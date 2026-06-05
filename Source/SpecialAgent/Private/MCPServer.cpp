// Copyright Epic Games, Inc. All Rights Reserved.

#include "MCPServer.h"
#include "MCPRequestRouter.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Guid.h"
#include "Async/Async.h"
#include "Misc/ConfigCacheIni.h"

FSpecialAgentMCPServer::FSpecialAgentMCPServer()
	: bIsRunning(false)
	, ServerPort(8767)
	, LastClientActivity(FDateTime::MinValue())
	, ServerBindAddress(TEXT("127.0.0.1"))
	, bEnforceLocalOnly(true)
{
	RequestRouter = MakeShared<FMCPRequestRouter>();
}

FSpecialAgentMCPServer::~FSpecialAgentMCPServer()
{
	StopServer();
}

bool FSpecialAgentMCPServer::StartServer(int32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("CERBEROUS-MCP+: MCP Server is already running"));
		return false;
	}

	ServerPort = Port;

	// Read configuration (non-sensitive settings only)
	if (GConfig)
	{
		// Respect existing /Script/SpecialAgent.SpecialAgentSettings section but add safer defaults
		GConfig->GetString(TEXT("/Script/SpecialAgent.SpecialAgentSettings"), TEXT("ServerBindAddress"), ServerBindAddress, GGameIni);
		GConfig->GetBool(TEXT("/Script/SpecialAgent.SpecialAgentSettings"), TEXT("bEnforceLocalOnly"), bEnforceLocalOnly, GGameIni);
	}

	// Enforce local-only by default for safety. If bEnforceLocalOnly is true, force loopback bind address.
	if (bEnforceLocalOnly)
	{
		if (!ServerBindAddress.IsEmpty() && !(ServerBindAddress == TEXT("127.0.0.1") || ServerBindAddress == TEXT("::1") || ServerBindAddress.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)))
		{
			UE_LOG(LogTemp, Warning, TEXT("CERBEROUS-MCP+: bEnforceLocalOnly is true — overriding configured ServerBindAddress (%s) to loopback (127.0.0.1) for safety"), *ServerBindAddress);
			ServerBindAddress = TEXT("127.0.0.1");
		}
	}

	// Get the HTTP server module
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	
	// Start listeners on the specified port
	// Note: The underlying HttpServerModule typically binds based on engine-level settings.
	// We call StartAllListeners() to initialize listeners; ensure platform firewall rules allow loopback only as needed.
	HttpServerModule.StartAllListeners();
	
	// Get the HTTP router for our port
	HttpRouter = HttpServerModule.GetHttpRouter(ServerPort);
	
	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("CERBEROUS-MCP+: Failed to get HTTP router for port %d"), ServerPort);
		return false;
	}

	// Register MCP endpoint (POST /mcp) - Main streamable HTTP endpoint
	HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleMessage)
	);

	// Register SSE endpoint (GET /sse) - For SSE transport fallback
	SSERouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/sse")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleSSEConnection)
	);
	
	// Also handle POST on /sse for streamable HTTP transport
	HttpRouter->BindRoute(
		FHttpPath(TEXT("/sse")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleMessage)
	);

	// Register message endpoint (POST /message)
	MessageRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/message")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleMessage)
	);

	// Register health endpoint (GET /health)
	HealthRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleHealth)
	);

	// Register OPTIONS handlers for CORS preflight on all endpoints
	HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleCORS)
	);
	
	HttpRouter->BindRoute(
		FHttpPath(TEXT("/sse")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleCORS)
	);
	
	HttpRouter->BindRoute(
		FHttpPath(TEXT("/message")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FSpecialAgentMCPServer::HandleCORS)
	);

	bIsRunning = true;
	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: MCP HTTP Server started on port %d (bind=%s)"), ServerPort, *ServerBindAddress);
	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: SSE endpoint: http://%s:%d/sse"), *ServerBindAddress, ServerPort);
	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: Message endpoint: http://%s:%d/message"), *ServerBindAddress, ServerPort);
	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: Health endpoint: http://%s:%d/health"), *ServerBindAddress, ServerPort);

	return true;
}

void FSpecialAgentMCPServer::StopServer()
{
	if (!bIsRunning)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: MCP Server stopping"));

	// Unbind routes
	if (HttpRouter.IsValid())
	{
		HttpRouter->UnbindRoute(SSERouteHandle);
		HttpRouter->UnbindRoute(MessageRouteHandle);
		HttpRouter->UnbindRoute(HealthRouteHandle);
	}

	// Clear connections
	{
		FScopeLock Lock(&ConnectionsLock);
		SSEConnections.Empty();
	}

	bIsRunning = false;
	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: MCP Server stopped"));
}

FString FSpecialAgentMCPServer::GenerateSessionId()
{
	return FGuid::NewGuid().ToString();
}

bool FSpecialAgentMCPServer::HandleSSEConnection(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Optionally enforce local-only client connections based on config
	if (bEnforceLocalOnly)
	{
		// Check for common proxy headers for client address; if present and not loopback, reject.
		const FString* XFF = Request.Headers.Find(TEXT("X-Forwarded-For"));
		const FString* XRI = Request.Headers.Find(TEXT("X-Real-IP"));
		FString ClientAddr;
		if (XFF && !XFF->IsEmpty())
		{
			ClientAddr = XFF->LeftChop(0);
		}
		else if (XRI && !XRI->IsEmpty())
		{
			ClientAddr = *XRI;
		}

		if (!ClientAddr.IsEmpty() && !(ClientAddr == TEXT("127.0.0.1") || ClientAddr == TEXT("::1") || ClientAddr.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)))
		{
			UE_LOG(LogTemp, Warning, TEXT("CERBEROUS-MCP+: Rejecting SSE connection from non-local address: %s"), *ClientAddr);
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(TEXT("Forbidden"), TEXT("text/plain"));
			Response->Code = EHttpServerResponseCodes::Forbidden;
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: New SSE connection request"));

	// Generate session ID for this connection
	FString SessionId = GenerateSessionId();

	// Record client activity
	RecordClientActivity();

	// Build the SSE response with the endpoint event
	// MCP SSE transport expects: event: endpoint\ndata: <url>\n\n
	FString MessageEndpoint = FString::Printf(TEXT("http://%s:%d/message?sessionId=%s"), *ServerBindAddress, ServerPort, *SessionId);
	
	// Format as proper SSE event
	FString SSEData = FString::Printf(
		TEXT("event: endpoint\ndata: %s\n\n"),
		*MessageEndpoint
	);

	// Create response with SSE content type
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(TEXT(""), TEXT("text/event-stream"));
	
	// Set required SSE headers
	Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache, no-store, must-revalidate") });
	Response->Headers.Add(TEXT("Connection"), { TEXT("keep-alive") });
	Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
	Response->Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, POST, OPTIONS") });
	Response->Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Content-Type, Accept") });
	Response->Headers.Add(TEXT("X-Accel-Buffering"), { TEXT("no") });
	
	// Add the SSE event data
	FTCHARToUTF8 Converter(*SSEData);
	Response->Body.Append((const uint8*)Converter.Get(), Converter.Length());

	Response->Code = EHttpServerResponseCodes::Ok;

	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: SSE endpoint event sent, session: %s, endpoint: %s"), *SessionId, *MessageEndpoint);

	OnComplete(MoveTemp(Response));

	return true;
}

bool FSpecialAgentMCPServer::HandleMessage(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// If configured to enforce local-only clients, attempt to detect non-local clients via common headers and reject.
	if (bEnforceLocalOnly)
	{
		const FString* XFF = Request.Headers.Find(TEXT("X-Forwarded-For"));
		const FString* XRI = Request.Headers.Find(TEXT("X-Real-IP"));
		FString ClientAddr;
		if (XFF && !XFF->IsEmpty())
		{
			ClientAddr = XFF->LeftChop(0);
		}
		else if (XRI && !XRI->IsEmpty())
		{
			ClientAddr = *XRI;
		}

		if (!ClientAddr.IsEmpty() && !(ClientAddr == TEXT("127.0.0.1") || ClientAddr == TEXT("::1") || ClientAddr.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)))
		{
			UE_LOG(LogTemp, Warning, TEXT("CERBEROUS-MCP+: Rejecting message POST from non-local address: %s"), *ClientAddr);
			TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
			ErrorObj->SetStringField(TEXT("error"), TEXT("Forbidden: non-local client"));

			FString ResponseJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseJson);
			FJsonSerializer::Serialize(ErrorObj.ToSharedRef(), Writer);

			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseJson, TEXT("application/json"));
			Response->Code = EHttpServerResponseCodes::Forbidden;
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	// Get session ID from query parameters (optional)
	FString SessionId;
	const FString* SessionIdValue = Request.QueryParams.Find(TEXT("sessionId"));
	if (SessionIdValue)
	{
		SessionId = *SessionIdValue;
	}

	// Get request body - handle potentially empty or malformed data
	FString BodyString;
	if (Request.Body.Num() > 0)
	{
		// Ensure null termination for string conversion
		TArray<uint8> BodyWithNull = Request.Body;
		BodyWithNull.Add(0);
		BodyString = UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyWithNull.GetData()));
	}
	
	UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: Received message (session: %s, size: %d): %s"), 
		*SessionId, Request.Body.Num(), *BodyString.Left(1000));

	// Record client activity for status tracking
	RecordClientActivity();

	// Handle empty body - some clients send empty POST to check connection
	if (BodyString.IsEmpty() || BodyString.TrimStartAndEnd().IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("CERBEROUS-MCP+: Received empty request body"));
		
		// Return a simple acknowledgment for empty requests
		TSharedPtr<FJsonObject> AckResult = MakeShared<FJsonObject>();
		AckResult->SetStringField(TEXT("status"), TEXT("ready"));
		AckResult->SetStringField(TEXT("server"), TEXT("CERBEROUS-MCP+"));
		
		FString ResponseJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseJson);
		FJsonSerializer::Serialize(AckResult.ToSharedRef(), Writer);
		
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseJson, TEXT("application/json"));
		Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		Response->Code = EHttpServerResponseCodes::Ok;
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Parse the JSON-RPC request
	FMCPRequest MCPRequest;
	if (!ParseRequest(BodyString, MCPRequest))
	{
		UE_LOG(LogTemp, Error, TEXT("CERBEROUS-MCP+: Failed to parse JSON: %s"), *BodyString.Left(500));
		
		FMCPResponse ErrorResponse = FMCPResponse::Error(
			TEXT(""),
			-32700,
			TEXT("Parse error: Invalid JSON")
		);

		FString ResponseJson = FormatResponse(ErrorResponse);
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseJson, TEXT("application/json"));
		Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		Response->Code = EHttpServerResponseCodes::BadRequest;
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Process on game thread and send response
	AsyncTask(ENamedThreads::GameThread, [this, MCPRequest, OnComplete, SessionId]()
	{
		UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: Processing request on game thread: %s"), *MCPRequest.Method);
		
		FMCPResponse MCPResponse = RequestRouter->RouteRequest(MCPRequest);
		
		UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: RouteRequest completed for: %s"), *MCPRequest.Method);
		
		FString ResponseJson = FormatResponse(MCPResponse);

		UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: Response ready for %s (size=%d): %s"), 
			*MCPRequest.Method, ResponseJson.Len(), *ResponseJson.Left(300));

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseJson, TEXT("application/json"));
		Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, POST, OPTIONS") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Content-Type") });
		Response->Code = EHttpServerResponseCodes::Ok;

		UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: Calling OnComplete for: %s"), *MCPRequest.Method);
		OnComplete(MoveTemp(Response));
		UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: OnComplete returned for: %s"), *MCPRequest.Method);
	});

	return true;
}

bool FSpecialAgentMCPServer::HandleCORS(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(TEXT(""), TEXT("text/plain"));
	Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
	Response->Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, POST, OPTIONS") });
	Response->Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Content-Type, Accept, Authorization") });
	Response->Headers.Add(TEXT("Access-Control-Max-Age"), { TEXT("86400") });
	Response->Code = EHttpServerResponseCodes::NoContent;
	OnComplete(MoveTemp(Response));
	return true;
}

bool FSpecialAgentMCPServer::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> HealthObj = MakeShared<FJsonObject>();
	HealthObj->SetStringField(TEXT("status"), TEXT("healthy"));
	HealthObj->SetStringField(TEXT("server"), TEXT("CERBEROUS-MCP+ MCP Server"));
	HealthObj->SetStringField(TEXT("version"), TEXT("1.0.0"));
	HealthObj->SetNumberField(TEXT("port"), ServerPort);
	HealthObj->SetBoolField(TEXT("running"), bIsRunning);

	FString ResponseJson;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseJson);
	FJsonSerializer::Serialize(HealthObj.ToSharedRef(), Writer);

	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseJson, TEXT("application/json"));
	Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
	Response->Code = EHttpServerResponseCodes::Ok;

	OnComplete(MoveTemp(Response));
	return true;
}

bool FSpecialAgentMCPServer::ParseRequest(const FString& JsonString, FMCPRequest& OutRequest)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return false;
	}

	// Parse JSON-RPC fields
	// Strict validation: jsonrpc must be "2.0"
	FString JsonRpcVal;
	if (!JsonObject->TryGetStringField(TEXT("jsonrpc"), JsonRpcVal) || JsonRpcVal != TEXT("2.0"))
	{
		return false;
	}
	OutRequest.JsonRpc = JsonRpcVal;

	FString MethodVal;
	if (!JsonObject->TryGetStringField(TEXT("method"), MethodVal) || MethodVal.IsEmpty())
	{
		return false;
	}
	OutRequest.Method = MethodVal;
	
	// Params can be object or omitted; enforce object when present
	const TSharedPtr<FJsonObject>* ParamsObj;
	if (JsonObject->TryGetObjectField(TEXT("params"), ParamsObj))
	{
		OutRequest.Params = *ParamsObj;
	}
	else
	{
		OutRequest.Params = MakeShared<FJsonObject>();
	}

	// ID can be string or number
	const TSharedPtr<FJsonValue> IdValue = JsonObject->TryGetField(TEXT("id"));
	if (IdValue.IsValid())
	{
		if (IdValue->Type == EJson::String)
		{
			OutRequest.Id = IdValue->AsString();
		}
		else if (IdValue->Type == EJson::Number)
		{
			OutRequest.Id = FString::Printf(TEXT("%d"), (int32)IdValue->AsNumber());
		}
		else
		{
			// Non-string/number ids are not supported
			return false;
		}
	}

	return true;
}

FString FSpecialAgentMCPServer::FormatResponse(const FMCPResponse& Response)
{
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

	JsonObject->SetStringField(TEXT("jsonrpc"), Response.JsonRpc);
	
	// Handle ID - can be string or number based on what was sent
	if (!Response.Id.IsEmpty())
	{
		// Try to parse as number first
		if (Response.Id.IsNumeric())
		{
			JsonObject->SetNumberField(TEXT("id"), FCString::Atoi(*Response.Id));
		}
		else
		{
			JsonObject->SetStringField(TEXT("id"), Response.Id);
		}
	}
	else
	{
		JsonObject->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}

	if (Response.bSuccess && Response.Result.IsValid())
	{
		JsonObject->SetObjectField(TEXT("result"), Response.Result);
	}
	else if (!Response.bSuccess && Response.ErrorObject.IsValid())
	{
		JsonObject->SetObjectField(TEXT("error"), Response.ErrorObject);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

	return OutputString;
}

void FSpecialAgentMCPServer::SendSSEEvent(const FString& SessionId, const FString& EventType, const FString& Data)
{
	FScopeLock Lock(&ConnectionsLock);

	TSharedPtr<FSSEConnection>* ConnectionPtr = SSEConnections.Find(SessionId);
	if (ConnectionPtr && (*ConnectionPtr)->bIsValid)
	{
		FString EventData = FString::Printf(TEXT("event: %s\ndata: %s\n\n"), *EventType, *Data);
		UE_LOG(LogTemp, Verbose, TEXT("CERBEROUS-MCP+: Sending SSE event to %s: %s"), *SessionId, *EventType);
	}
}

void FSpecialAgentMCPServer::BroadcastSSEEvent(const FString& EventType, const FString& Data)
{
	FScopeLock Lock(&ConnectionsLock);

	for (auto& Pair : SSEConnections)
	{
		if (Pair.Value->bIsValid)
		{
			SendSSEEvent(Pair.Key, EventType, Data);
		}
	}
}

void FSpecialAgentMCPServer::CleanupConnections()
{
	FScopeLock Lock(&ConnectionsLock);

	TArray<FString> ToRemove;
	for (auto& Pair : SSEConnections)
	{
		if (!Pair.Value->bIsValid)
		{
			ToRemove.Add(Pair.Key);
		}
	}

	for (const FString& Key : ToRemove)
	{
		SSEConnections.Remove(Key);
		UE_LOG(LogTemp, Log, TEXT("CERBEROUS-MCP+: Cleaned up stale SSE connection: %s"), *Key);
	}
}

int32 FSpecialAgentMCPServer::GetConnectedClientCount() const
{
	if (!bIsRunning)
	{
		return 0;
	}

	// Consider a client "connected" if we've received activity recently
	FTimespan TimeSinceActivity = FDateTime::Now() - LastClientActivity;
	if (TimeSinceActivity.GetTotalSeconds() < ClientActivityTimeoutSeconds)
	{
		return 1; // At least one active client
	}

	return 0;
}

void FSpecialAgentMCPServer::RecordClientActivity()
{
	LastClientActivity = FDateTime::Now();
}
