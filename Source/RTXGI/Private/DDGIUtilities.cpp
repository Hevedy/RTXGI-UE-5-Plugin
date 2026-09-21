/*
* Copyright (c) 2019-2021, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "../Public/DDGIUtilities.h"

//Implementation from FLatentGPUTimer from ScenePrivate.h / RendererScene.cpp
FLatentGPUTimerDDGI::FLatentGPUTimerDDGI(FRenderQueryPoolRHIRef InTimerQueryPool)
	: TimerQueryPool(InTimerQueryPool)
	, AvgSamples(30)
	, TotalTime(0.0f)
	, SampleIndex(0)
	, QueryIndex(0)
{
	TimeSamples.AddZeroed(AvgSamples);
}

void FLatentGPUTimerDDGI::SetPool(FRenderQueryPoolRHIRef InTimerQueryPool)
{
	TimerQueryPool = InTimerQueryPool;
}

bool FLatentGPUTimerDDGI::Tick(FRHICommandListImmediate& RHICmdList)
{
	if (GSupportsTimestampRenderQueries == false)
	{
		return false;
	}

	QueryIndex = (QueryIndex + 1) % NumBufferedFrames;

	if (StartQueries[QueryIndex].GetQuery() && EndQueries[QueryIndex].GetQuery())
	{
		if (IsRunningRHIInSeparateThread())
		{
			// Block until the RHI thread has processed the previous query commands, if necessary
			// Stat disabled since we buffer 2 frames minimum, it won't actually block
			//SCOPE_CYCLE_COUNTER(STAT_TranslucencyTimestampQueryFence_Wait);
			int32 BlockFrame = NumBufferedFrames - 1;
			FRHICommandListExecutor::WaitOnRHIThreadFence(QuerySubmittedFences[BlockFrame]);
			QuerySubmittedFences[BlockFrame] = nullptr;
		}

		uint64 StartMicroseconds;
		uint64 EndMicroseconds;
		bool bStartSuccess;
		bool bEndSuccess;

		{
			// Block on the GPU until we have the timestamp query results, if necessary
			// Stat disabled since we buffer 2 frames minimum, it won't actually block
			//SCOPE_CYCLE_COUNTER(STAT_TranslucencyTimestampQuery_Wait);
			bStartSuccess = RHIGetRenderQueryResult(StartQueries[QueryIndex].GetQuery(), StartMicroseconds, true);
			bEndSuccess = RHIGetRenderQueryResult(EndQueries[QueryIndex].GetQuery(), EndMicroseconds, true);
		}

		TotalTime -= TimeSamples[SampleIndex];
		float LastFrameTranslucencyDurationMS = TimeSamples[SampleIndex];
		if (bStartSuccess && bEndSuccess)
		{
			LastFrameTranslucencyDurationMS = (EndMicroseconds - StartMicroseconds) / 1000.0f;
		}

		TimeSamples[SampleIndex] = LastFrameTranslucencyDurationMS;
		TotalTime += LastFrameTranslucencyDurationMS;
		SampleIndex = (SampleIndex + 1) % AvgSamples;

		return bStartSuccess && bEndSuccess;
	}

	return false;
}

void FLatentGPUTimerDDGI::Begin(FRHICommandListImmediate& RHICmdList)
{
	if (GSupportsTimestampRenderQueries == false)
	{
		return;
	}

	if (!StartQueries[QueryIndex].GetQuery())
	{
		StartQueries[QueryIndex] = TimerQueryPool->AllocateQuery();
	}

	RHICmdList.EndRenderQuery(StartQueries[QueryIndex].GetQuery());
}

void FLatentGPUTimerDDGI::End(FRHICommandListImmediate& RHICmdList)
{
	if (GSupportsTimestampRenderQueries == false)
	{
		return;
	}

	if (!EndQueries[QueryIndex].GetQuery())
	{
		EndQueries[QueryIndex] = TimerQueryPool->AllocateQuery();
	}

	RHICmdList.EndRenderQuery(EndQueries[QueryIndex].GetQuery());
	// UE 5.8 removed SubmitCommandsHint(). Dispatch the pending commands explicitly instead.
	// When an RHI thread is present, enqueue a fence first so query result polling can verify
	// that EndRenderQuery has reached the RHI thread.
	if (IsRunningRHIInSeparateThread())
	{
		int32 NumFrames = NumBufferedFrames;
		for (int32 Dest = 1; Dest < NumFrames; Dest++)
		{
			QuerySubmittedFences[Dest] = QuerySubmittedFences[Dest - 1];
		}
		QuerySubmittedFences[0] = RHICmdList.RHIThreadFence();
	}

	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
}

void FLatentGPUTimerDDGI::Release()
{
	for (int32 i = 0; i < NumBufferedFrames; ++i)
	{
		StartQueries[i].ReleaseQuery();
		EndQueries[i].ReleaseQuery();
	}
}

float FLatentGPUTimerDDGI::GetTimeMS()
{
	return TimeSamples[SampleIndex];
}

float FLatentGPUTimerDDGI::GetAverageTimeMS()
{
	return TotalTime / AvgSamples;
}