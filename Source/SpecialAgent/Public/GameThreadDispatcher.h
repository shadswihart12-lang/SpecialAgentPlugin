// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Async/Async.h"

/**
 * Game Thread Dispatcher
 * 
 * Utility class for dispatching tasks to the game thread and waiting for results.
 * Essential for thread-safe access to UE5 APIs from the MCP server thread.
 */
class SPECIALAGENT_API FGameThreadDispatcher
{
public:
	/**
	 * Dispatch a task to the game thread and return a future for the result
	 * @param Task The task to execute on the game thread
	 * @return A future that will contain the result
	 */
	template<typename ReturnType, typename Fun>
	static TFuture<ReturnType> DispatchToGameThread(Fun Task)
	{
		TPromise<ReturnType> Promise;
		TFuture<ReturnType> Future = Promise.GetFuture();

		// Use a templated callable passed by value to avoid TFunction heap allocations in hot paths
		AsyncTask(ENamedThreads::GameThread, [Task = MoveTemp(Task), Promise = MoveTemp(Promise)]() mutable
		{
			ReturnType Result = Task();
			Promise.SetValue(MoveTemp(Result));
		});

		return Future;
	}

	/**
	 * Dispatch a task to the game thread and wait synchronously for completion
	 * @param Task The task to execute on the game thread
	 */
	template<typename Fun>
	static void DispatchToGameThreadSync(Fun Task)
	{
		if (IsInGameThread())
		{
			// Already on game thread, execute directly
			Task();
		}
		else
		{
			// Dispatch to game thread and wait
			TPromise<void> Promise;
			TFuture<void> Future = Promise.GetFuture();

			AsyncTask(ENamedThreads::GameThread, [Task = MoveTemp(Task), Promise = MoveTemp(Promise)]() mutable
			{
				Task();
				Promise.SetValue();
			});

			// Wait for completion - callers should avoid holding locks while calling this
			Future.Wait();
		}
	}

	/**
	 * Dispatch a task with a return value to the game thread and wait synchronously
	 * @param Task The task to execute on the game thread
	 * @return The result of the task
	 */
	template<typename ReturnType, typename Fun>
	static ReturnType DispatchToGameThreadSyncWithReturn(Fun Task)
	{
		if (IsInGameThread())
		{
			// Already on game thread, execute directly
			return Task();
		}
		else
		{
			// Dispatch to game thread and wait. Prefer async usage where possible to avoid blocking.
			TFuture<ReturnType> Future = DispatchToGameThread<ReturnType, Fun>(MoveTemp(Task));
			return Future.Get();
		}
	}
};
