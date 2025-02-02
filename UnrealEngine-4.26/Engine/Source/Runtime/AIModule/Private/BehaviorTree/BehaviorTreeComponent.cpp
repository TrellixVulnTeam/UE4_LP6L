// Copyright Epic Games, Inc. All Rights Reserved.

#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "VisualLogger/VisualLoggerTypes.h"
#include "VisualLogger/VisualLogger.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTreeDelegates.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeManager.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Tasks/BTTask_RunBehaviorDynamic.h"
#include "ProfilingDebugging/ScopedTimers.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ConfigCacheIni.h"


#if USE_BEHAVIORTREE_DEBUGGER
int32 UBehaviorTreeComponent::ActiveDebuggerCounter = 0;
#endif

// Code for timing BT Search
static TAutoConsoleVariable<int32> CVarBTRecordFrameSearchTimes(TEXT("BehaviorTree.RecordFrameSearchTimes"), 0, TEXT("Record Search Times Per Frame For Perf Stats"));
#if !UE_BUILD_SHIPPING
bool UBehaviorTreeComponent::bAddedEndFrameCallback = false;
double UBehaviorTreeComponent::FrameSearchTime = 0.;
int32 UBehaviorTreeComponent::NumSearchTimeCalls = 0;
#endif

struct FScopedBehaviorTreeLock
{
	FScopedBehaviorTreeLock(UBehaviorTreeComponent& InOwnerComp, uint8 InLockFlag) : OwnerComp(InOwnerComp), LockFlag(InLockFlag)
	{
		OwnerComp.StopTreeLock |= LockFlag;
	}

	~FScopedBehaviorTreeLock()
	{
		OwnerComp.StopTreeLock &= ~LockFlag;
	}

	enum
	{
		LockTick = 1 << 0,
		LockReentry = 1 << 1,
	};

private:
	UBehaviorTreeComponent& OwnerComp;
	uint8 LockFlag;
};

//----------------------------------------------------------------------//
// UBehaviorTreeComponent
//----------------------------------------------------------------------//

UBehaviorTreeComponent::UBehaviorTreeComponent(const FObjectInitializer& ObjectInitializer) 
	: Super(ObjectInitializer)
	, SearchData(*this)
{
	ActiveInstanceIdx = 0;
	StopTreeLock = 0;
	bDeferredStopTree = false;
	bLoopExecution = false;
	bWaitingForAbortingTasks = false;
	bRequestedFlowUpdate = false;
	bAutoActivate = true;
	bWantsInitializeComponent = true; 
	bIsRunning = false;
	bIsPaused = false;

	// Adding hook for bespoke framepro BT timings for BR
#if !UE_BUILD_SHIPPING
	if (!bAddedEndFrameCallback)
	{
		bAddedEndFrameCallback = true;
		FCoreDelegates::OnEndFrame.AddStatic(&UBehaviorTreeComponent::EndFrame);
	}
#endif
}

UBehaviorTreeComponent::UBehaviorTreeComponent(FVTableHelper& Helper)
	: Super(Helper)
	, SearchData(*this)
{

}

// 取消初始化
void UBehaviorTreeComponent::UninitializeComponent()
{
	// 从行为树管理器删除
	UBehaviorTreeManager* BTManager = UBehaviorTreeManager::GetCurrent(GetWorld());
	if (BTManager)
	{
		BTManager->RemoveActiveComponent(*this);
	}

	RemoveAllInstances();
	Super::UninitializeComponent();
}

// 注册组件的tick函数
void UBehaviorTreeComponent::RegisterComponentTickFunctions(bool bRegister)
{
	if (bRegister)
	{
		ScheduleNextTick(0.0f);
	}
	Super::RegisterComponentTickFunctions(bRegister);
}

//  设置组件的tick可用
void UBehaviorTreeComponent::SetComponentTickEnabled(bool bEnabled)
{
	// 是否启用Tick
	bool bWasEnabled = IsComponentTickEnabled();
	Super::SetComponentTickEnabled(bEnabled);

	// If enabling the component, this acts like a new component to tick in the TickTaskManager
	// So act like the component was never ticked
	// 如果启用该组件，这种行为就像一个新的组件在TickTaskManager中tick
	// 所以行为就像该组件从来没有ticked
	if(!bWasEnabled && IsComponentTickEnabled())
	{
		bTickedOnce = false;
		ScheduleNextTick(0.0f);
	}
}

// 开始逻辑
void UBehaviorTreeComponent::StartLogic()
{
	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("%s"), ANSI_TO_TCHAR(__FUNCTION__));

	if (TreeHasBeenStarted())
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("%s: Skipping, logic already started."), ANSI_TO_TCHAR(__FUNCTION__));
		return;
	}

	// 开始信息没有设置树的资产，直接用默认的
	if (TreeStartInfo.IsSet() == false)
	{
		TreeStartInfo.Asset = DefaultBehaviorTreeAsset;
	}

	if (TreeStartInfo.IsSet())
	{
		TreeStartInfo.bPendingInitialize = true;
		ProcessPendingInitialize();
	}
	else
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("%s: Could not find BehaviorTree asset to run."), ANSI_TO_TCHAR(__FUNCTION__));
	}
}

// 重启逻辑
void UBehaviorTreeComponent::RestartLogic()
{
	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("%s"), ANSI_TO_TCHAR(__FUNCTION__));
	RestartTree();
}

// 结束逻辑
void UBehaviorTreeComponent::StopLogic(const FString& Reason) 
{
	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Stopping BT, reason: \'%s\'"), *Reason);
	StopTree(EBTStopMode::Safe);
}

// 暂停逻辑
void UBehaviorTreeComponent::PauseLogic(const FString& Reason)
{
	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Execution updates: PAUSED (%s)"), *Reason);
	bIsPaused = true;

	if (BlackboardComp)
	{
		BlackboardComp->PauseObserverNotifications();
	}
}

// 恢复逻辑
EAILogicResuming::Type UBehaviorTreeComponent::ResumeLogic(const FString& Reason)
{
	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Execution updates: RESUMED (%s)"), *Reason);
	const EAILogicResuming::Type SuperResumeResult = Super::ResumeLogic(Reason);
	// 如果是暂停状态
	if (!!bIsPaused)
	{
		// 取消暂停，并在下一帧调用tick函数
		bIsPaused = false;
		ScheduleNextTick(0.0f);

		// 继续
		if (SuperResumeResult == EAILogicResuming::Continue)
		{
			if (BlackboardComp)
			{
				// Resume the blackboard's observer notifications and send any queued notifications
				// 恢复黑板的观察者通知并且发送任何排队通知
				BlackboardComp->ResumeObserverNotifications(true);
			}

			const bool bOutOfNodesPending = PendingExecution.IsSet() && PendingExecution.bOutOfNodes;
			if (ExecutionRequest.ExecuteNode || bOutOfNodesPending)
			{
				ScheduleExecutionUpdate();
			}

			return EAILogicResuming::Continue;
		}
		// 重启
		else if (SuperResumeResult == EAILogicResuming::RestartedInstead)
		{
			if (BlackboardComp)
			{
				// Resume the blackboard's observer notifications but do not send any queued notifications
				BlackboardComp->ResumeObserverNotifications(false);
			}
		}
	}

	return SuperResumeResult;
}

// 表示实例已被初始化以使用特定的BT资产（树已经开始执行）
bool UBehaviorTreeComponent::TreeHasBeenStarted() const
{
	return bIsRunning && InstanceStack.Num();
}

// 是否执行中（已经开始执行并且没有暂停）
bool UBehaviorTreeComponent::IsRunning() const
{ 
	return bIsPaused == false && TreeHasBeenStarted() == true;
}

// 是否已经暂停
bool UBehaviorTreeComponent::IsPaused() const
{
	return bIsPaused;
}

// 从根开始执行
void UBehaviorTreeComponent::StartTree(UBehaviorTree& Asset, EBTExecutionMode::Type ExecuteMode /*= EBTExecutionMode::Looped*/)
{
	// clear instance stack, start should always run new tree from root
	// 清空实例堆栈，开始应始终从实例堆栈顶部运行新树
	UBehaviorTree* CurrentRoot = GetRootTree();

	// 如果运行的树是最根部的树，并且树已经开始了
	if (CurrentRoot == &Asset && TreeHasBeenStarted())
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Skipping behavior start request - it's already running"));
		return;
	}
	else if (CurrentRoot)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Abandoning behavior %s to start new one (%s)"),
			*GetNameSafe(CurrentRoot), *Asset.GetName());
	}

	// 停止树执行
	StopTree(EBTStopMode::Safe);

	// 设置启动信息
	TreeStartInfo.Asset = &Asset;
	TreeStartInfo.ExecuteMode = ExecuteMode;
	TreeStartInfo.bPendingInitialize = true;

	// 处理代办的初始化
	ProcessPendingInitialize();
}

// 应用代办的树初始化
void UBehaviorTreeComponent::ProcessPendingInitialize()
{
	// 停止树执行
	StopTree(EBTStopMode::Safe);
	if (bWaitingForAbortingTasks)
	{
		return;
	}

	// finish cleanup
	// 完成清除
	RemoveAllInstances();

	bLoopExecution = (TreeStartInfo.ExecuteMode == EBTExecutionMode::Looped);
	bIsRunning = true;

#if USE_BEHAVIORTREE_DEBUGGER
	DebuggerSteps.Reset();
#endif
	// 加入行为树管理器
	UBehaviorTreeManager* BTManager = UBehaviorTreeManager::GetCurrent(GetWorld());
	if (BTManager)
	{
		BTManager->AddActiveComponent(*this);
	}

	// push new instance
	// 压入新的实例
	const bool bPushed = PushInstance(*TreeStartInfo.Asset);
	TreeStartInfo.bPendingInitialize = false;
}

// 结束执行
void UBehaviorTreeComponent::StopTree(EBTStopMode::Type StopMode)
{
	SCOPE_CYCLE_COUNTER(STAT_AI_BehaviorTree_StopTree);
	// 树停止锁
	if (StopTreeLock)
	{
		// 延迟停止树标志，StopTree将会在tick得结尾调用
		bDeferredStopTree = true;
		ScheduleNextTick(0.0f);
		return;
	}

	FScopedBehaviorTreeLock ScopedLock(*this, FScopedBehaviorTreeLock::LockReentry);
	// 没有调用过Stop函数
	if (!bRequestedStop)
	{
		// 设置调用过Stop函数
		bRequestedStop = true;

		for (int32 InstanceIndex = InstanceStack.Num() - 1; InstanceIndex >= 0; InstanceIndex--)
		{
			FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];

			// notify active aux nodes
			// 通知激活的辅助节点，每一个都调用CeaseRelevant
			InstanceInfo.ExecuteOnEachAuxNode([&InstanceInfo, this](const UBTAuxiliaryNode& AuxNode)
				{
					uint8* NodeMemory = AuxNode.GetNodeMemory<uint8>(InstanceInfo);
					AuxNode.WrappedOnCeaseRelevant(*this, NodeMemory);
				});
			InstanceInfo.ResetActiveAuxNodes();

			// notify active parallel tasks
			//
			// calling OnTaskFinished with result other than InProgress will unregister parallel task,
			// modifying array we're iterating on - iterator needs to be moved one step back in that case
			//
			// 通知所有激活的平行任务
			// 调用带有结果的OnTaskFinished而不是InProgress将取消注册并行任务，
			// 修改我们正在遍历的数组，在这种情况下，迭代器需要回退一格
			InstanceInfo.ExecuteOnEachParallelTask([&InstanceInfo, this](const FBehaviorTreeParallelTask& ParallelTaskInfo, const int32 ParallelIndex)
				{
					if (ParallelTaskInfo.Status != EBTTaskStatus::Active)
					{
						return;
					}

					const UBTTaskNode* CachedTaskNode = ParallelTaskInfo.TaskNode;
					if (IsValid(CachedTaskNode) == false)
					{
						return;
					}

					// remove all message observers added by task execution, so they won't interfere with Abort call
					// 删除执行任务时增加的所有消息observers，这样他们就不会干扰中止调用
					UnregisterMessageObserversFrom(CachedTaskNode);

					// 中止任务
					uint8* NodeMemory = CachedTaskNode->GetNodeMemory<uint8>(InstanceInfo);
					EBTNodeResult::Type NodeResult = CachedTaskNode->WrappedAbortTask(*this, NodeMemory);

					UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Parallel task aborted: %s (%s)"),
						*UBehaviorTreeTypes::DescribeNodeHelper(CachedTaskNode),
						(NodeResult == EBTNodeResult::InProgress) ? TEXT("in progress") : TEXT("instant"));

					// mark as pending abort
					// 标记为代办的中止
					if (NodeResult == EBTNodeResult::InProgress)
					{
						const bool bIsValidForStatus = InstanceInfo.IsValidParallelTaskIndex(ParallelIndex) && (ParallelTaskInfo.TaskNode == CachedTaskNode);
						if (bIsValidForStatus)
						{
							InstanceInfo.MarkParallelTaskAsAbortingAt(ParallelIndex);
							bWaitingForAbortingTasks = true;
						}
						else
						{
							UE_VLOG(GetOwner(), LogBehaviorTree, Warning, TEXT("Parallel task %s was unregistered before completing Abort state!"),
								*UBehaviorTreeTypes::DescribeNodeHelper(CachedTaskNode));
						}
					}
					// 调用任务完成
					OnTaskFinished(CachedTaskNode, NodeResult);
				});

			// notify active task
			// 通知当前的活动任务
			if (InstanceInfo.ActiveNodeType == EBTActiveNode::ActiveTask)
			{
				const UBTTaskNode* TaskNode = Cast<const UBTTaskNode>(InstanceInfo.ActiveNode);
				check(TaskNode != NULL);

				// remove all observers before requesting abort
				UnregisterMessageObserversFrom(TaskNode);
				InstanceInfo.ActiveNodeType = EBTActiveNode::AbortingTask;

				UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Abort task: %s"), *UBehaviorTreeTypes::DescribeNodeHelper(TaskNode));

				// abort task using current state of tree
				uint8* NodeMemory = TaskNode->GetNodeMemory<uint8>(InstanceInfo);
				EBTNodeResult::Type TaskResult = TaskNode->WrappedAbortTask(*this, NodeMemory);

				// pass task finished if wasn't already notified (FinishLatentAbort)
				if (InstanceInfo.ActiveNodeType == EBTActiveNode::AbortingTask)
				{
					OnTaskFinished(TaskNode, TaskResult);
				}
			}
		}
	}

	// 等待中止的并行任务
	if (bWaitingForAbortingTasks)
	{
		if (StopMode == EBTStopMode::Safe)
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("StopTree is waiting for aborting tasks to finish..."));
			return;
		}

		UE_VLOG(GetOwner(), LogBehaviorTree, Warning, TEXT("StopTree was forced while waiting for tasks to finish aborting!"));
	}

	// make sure that all nodes are getting deactivation notifies
	// 确定所有的节点都获得停用的通知
	if (InstanceStack.Num())
	{
		int32 DeactivatedChildIndex = INDEX_NONE;
		EBTNodeResult::Type AbortedResult = EBTNodeResult::Aborted;
		DeactivateUpTo(InstanceStack[0].RootNode, 0, AbortedResult, DeactivatedChildIndex);
	}

	// clear current state, don't touch debugger data
	// 清空当前的状态
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		InstanceStack[InstanceIndex].Cleanup(*this, EBTMemoryClear::Destroy);
	}

	InstanceStack.Reset();
	TaskMessageObservers.Reset();
	SearchData.Reset();
	ExecutionRequest = FBTNodeExecutionInfo();
	PendingExecution = FBTPendingExecutionInfo();
	ActiveInstanceIdx = 0;

	// make sure to allow new execution requests
	// 确保允许新的执行请求
	bRequestedFlowUpdate = false;
	bRequestedStop = false;
	bIsRunning = false;
	bWaitingForAbortingTasks = false;
	bDeferredStopTree = false;
}

// 从根重新开始执行
void UBehaviorTreeComponent::RestartTree()
{
	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("%s"), ANSI_TO_TCHAR(__FUNCTION__));
	// 如不是运行状态
	if (!bIsRunning)
	{
		// 开始信息都设置好了，直接走初始化步骤
		if (TreeStartInfo.IsSet())
		{
			TreeStartInfo.bPendingInitialize = true;
			ProcessPendingInitialize();
		}
		else
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Warning, TEXT("\tFailed to restart tree logic since it has never been started and it\'s not possible to say which BT asset to use."));
		}
	}
	// 已经是运行状态了，并且是调用stop函数中
	else if (bRequestedStop)
	{
		TreeStartInfo.bPendingInitialize = true;
	}
	// 运行状态了，也不是stop函数中，那就从根开始执行
	else if (InstanceStack.Num())
	{
		FBehaviorTreeInstance& TopInstance = InstanceStack[0];
		RequestExecution(TopInstance.RootNode, 0, TopInstance.RootNode, -1, EBTNodeResult::Aborted);
	}
}

// 清理
void UBehaviorTreeComponent::Cleanup()
{
	SCOPE_CYCLE_COUNTER(STAT_AI_BehaviorTree_Cleanup);
	// 树停止
	StopTree(EBTStopMode::Forced);
	RemoveAllInstances();

	// 将树实例和堆栈实例都清空
	KnownInstances.Reset();
	InstanceStack.Reset();
	NodeInstances.Reset();
}

// 处理消息
void UBehaviorTreeComponent::HandleMessage(const FAIMessage& Message)
{
	Super::HandleMessage(Message);
	ScheduleNextTick(0.0f);
}

// 完成潜在的执行或中止
void UBehaviorTreeComponent::OnTaskFinished(const UBTTaskNode* TaskNode, EBTNodeResult::Type TaskResult)
{
	if (TaskNode == NULL || InstanceStack.Num() == 0 || IsPendingKill())
	{
		return;
	}

	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Task %s finished: %s"), 
		*UBehaviorTreeTypes::DescribeNodeHelper(TaskNode), *UBehaviorTreeTypes::DescribeNodeResult(TaskResult));

	// notify parent node
	UBTCompositeNode* ParentNode = TaskNode->GetParentNode();
	// 找到对应的树实例
	const int32 TaskInstanceIdx = FindInstanceContainingNode(TaskNode);
	if (!InstanceStack.IsValidIndex(TaskInstanceIdx))
	{
		return;
	}

	// 找到父节点内存
	uint8* ParentMemory = ParentNode->GetNodeMemory<uint8>(InstanceStack[TaskInstanceIdx]);

	const bool bWasWaitingForAbort = bWaitingForAbortingTasks;
	// 父节点通知子节点执行
	ParentNode->ConditionalNotifyChildExecution(*this, ParentMemory, *TaskNode, TaskResult);
	
	// 任务不是执行中
	if (TaskResult != EBTNodeResult::InProgress)
	{
		StoreDebuggerSearchStep(TaskNode, TaskInstanceIdx, TaskResult);

		// cleanup task observers
		// 清空任务的观察者
		UnregisterMessageObserversFrom(TaskNode);

		// notify task about it
		// 通知任务节点本身，任务已经完成
		uint8* TaskMemory = TaskNode->GetNodeMemory<uint8>(InstanceStack[TaskInstanceIdx]);
		TaskNode->WrappedOnTaskFinished(*this, TaskMemory, TaskResult);

		// update execution when active task is finished
		// 当活动任务完成更新执行
		if (InstanceStack.IsValidIndex(ActiveInstanceIdx) && InstanceStack[ActiveInstanceIdx].ActiveNode == TaskNode)
		{
			FBehaviorTreeInstance& ActiveInstance = InstanceStack[ActiveInstanceIdx];
			const bool bWasAborting = (ActiveInstance.ActiveNodeType == EBTActiveNode::AbortingTask);
			ActiveInstance.ActiveNodeType = EBTActiveNode::InactiveTask;

			// request execution from parent
			// 要求从父节点执行
			if (!bWasAborting)
			{
				RequestExecution(TaskResult);
			}
		}
		// 如果执行结果是中止
		else if (TaskResult == EBTNodeResult::Aborted && InstanceStack.IsValidIndex(TaskInstanceIdx) && InstanceStack[TaskInstanceIdx].ActiveNode == TaskNode)
		{
			// active instance may be already changed when getting back from AbortCurrentTask 
			// (e.g. new task is higher on stack)
			// 从AbortCurrentTask返回时，活动实例可能已经更改
			InstanceStack[TaskInstanceIdx].ActiveNodeType = EBTActiveNode::InactiveTask;
		}

		// update state of aborting tasks after currently finished one was set to Inactive
		// 当前完成的任务设置为“不活动”后，更新中止任务的状态 
		UpdateAbortingTasks();

		// make sure that we continue execution after all pending latent aborts finished
		// 确保所有潜在等待的中止任务都完成了，我们才继续执行
		if (!bWaitingForAbortingTasks && bWasWaitingForAbort)
		{
			if (bRequestedStop)
			{
				StopTree(EBTStopMode::Safe);
			}
			else
			{
				// force new search if there were any execution requests while waiting for aborting task
				// 等待中止任务时是否有任何执行请求，强制执行新搜索 
				if (ExecutionRequest.ExecuteNode)
				{
					UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> found valid ExecutionRequest, locking PendingExecution data to force new search!"));
					PendingExecution.Lock();

					if (ExecutionRequest.SearchEnd.IsSet())
					{
						UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> removing limit from end of search range! [abort done]"));
						ExecutionRequest.SearchEnd = FBTNodeIndex();
					}
				}

				ScheduleExecutionUpdate();
			}
		}
	}
	// 任务在执行中
	else
	{
		// always update state of aborting tasks
		// 更新中止任务的状态
		UpdateAbortingTasks();
	}

	// 是否有未完成的初始化
	if (TreeStartInfo.HasPendingInitialize())
	{
		// 处理未完成的初始化
		ProcessPendingInitialize();
	}
}

// 当树执行完所有的节点后调用
void UBehaviorTreeComponent::OnTreeFinished()
{
	UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("Ran out of nodes to check, %s tree."),
		bLoopExecution ? TEXT("looping") : TEXT("stopping"));

	// 激活的实例索引为最顶上的
	ActiveInstanceIdx = 0;
	StoreDebuggerExecutionStep(EBTExecutionSnap::OutOfNodes);

	if (bLoopExecution && InstanceStack.Num())
	{
		// it should be already deactivated (including root)
		// set active node to initial state: root activation
		// 它应该已经被停用（包括root）
		// 将活动节点设置为初始状态：根激活 
		FBehaviorTreeInstance& TopInstance = InstanceStack[0];
		TopInstance.ActiveNode = NULL;
		TopInstance.ActiveNodeType = EBTActiveNode::Composite;

		// make sure that all active aux nodes will be removed
		// root level services are being handled on applying search data
		// 确保所有活动的辅助节点都将被删除
		// 在应用搜索数据时正在处理根级服务 
		UnregisterAuxNodesUpTo(FBTNodeIndex(0, 0));

		// result doesn't really matter, root node will be reset and start iterating child nodes from scratch
		// although it shouldn't be set to Aborted, as it has special meaning in RequestExecution (switch to higher priority)
		// 结果并不重要，根节点将被重置并从头开始迭代子节点
		// 尽管不应将其设置为“中止”，因为它在RequestExecution中具有特殊含义（切换到更高的优先级） 
		RequestExecution(TopInstance.RootNode, 0, TopInstance.RootNode, 0, EBTNodeResult::InProgress);
	}
	else
	{
		// 不是循环执行就停止执行
		StopTree(EBTStopMode::Safe);
	}
}

// 如果激活节点是给定节点的一个子节点就返回true
bool UBehaviorTreeComponent::IsExecutingBranch(const UBTNode* Node, int32 ChildIndex) const
{
	// 找到节点包含的实例索引
	const int32 TestInstanceIdx = FindInstanceContainingNode(Node);
	if (!InstanceStack.IsValidIndex(TestInstanceIdx) || InstanceStack[TestInstanceIdx].ActiveNode == NULL)
	{
		return false;
	}

	// is it active node or root of tree?
	// 是否是活动节点或者树的根节点
	const FBehaviorTreeInstance& TestInstance = InstanceStack[TestInstanceIdx];
	if (Node == TestInstance.RootNode || Node == TestInstance.ActiveNode)
	{
		return true;
	}

	// compare with index of next child
	// 比较下一个子节点的索引
	const uint16 ActiveExecutionIndex = TestInstance.ActiveNode->GetExecutionIndex();
	const uint16 NextChildExecutionIndex = Node->GetParentNode()->GetChildExecutionIndex(ChildIndex + 1);
	return (ActiveExecutionIndex >= Node->GetExecutionIndex()) && (ActiveExecutionIndex < NextChildExecutionIndex);
}

// 如果辅助节点是当前的活动节点，就返回true
bool UBehaviorTreeComponent::IsAuxNodeActive(const UBTAuxiliaryNode* AuxNode) const
{
	if (AuxNode == NULL)
	{
		return false;
	}

	const uint16 AuxExecutionIndex = AuxNode->GetExecutionIndex();
	// 遍历所有的树实例
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		// 遍历树实例的激活辅助节点
		for (const UBTAuxiliaryNode* TestAuxNode : InstanceInfo.GetActiveAuxNodes())
		{
			// check template version
			if (TestAuxNode == AuxNode)
			{
				return true;
			}

			// check instanced version
			CA_SUPPRESS(6011);
			// 检查实例版本，如果允许索引一致，并且节点一致，就返回true
			if (AuxNode->IsInstanced() && TestAuxNode && TestAuxNode->GetExecutionIndex() == AuxExecutionIndex)
			{
				const uint8* NodeMemory = TestAuxNode->GetNodeMemory<uint8>(InstanceInfo);
				UBTNode* NodeInstance = TestAuxNode->GetNodeInstance(*this, (uint8*)NodeMemory);

				if (NodeInstance == AuxNode)
				{
					return true;
				}
			}
		}
	}

	return false;
}

// 如果辅助节点是当前的活动节点，就返回true
bool UBehaviorTreeComponent::IsAuxNodeActive(const UBTAuxiliaryNode* AuxNodeTemplate, int32 InstanceIdx) const
{
	return InstanceStack.IsValidIndex(InstanceIdx) && InstanceStack[InstanceIdx].GetActiveAuxNodes().Contains(AuxNodeTemplate);
}

// 返回指定任务节点的状态
EBTTaskStatus::Type UBehaviorTreeComponent::GetTaskStatus(const UBTTaskNode* TaskNode) const
{
	EBTTaskStatus::Type Status = EBTTaskStatus::Inactive;
	// 得到任务节点对应的树实例索引
	const int32 InstanceIdx = FindInstanceContainingNode(TaskNode);

	if (InstanceStack.IsValidIndex(InstanceIdx))
	{
		const uint16 ExecutionIndex = TaskNode->GetExecutionIndex();
		const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIdx];

		// always check parallel execution first, it takes priority over ActiveNodeType
		// 总是先检查并行任务优先，如果是并行任务，返回并行任务的状态
		for (const FBehaviorTreeParallelTask& ParallelInfo : InstanceInfo.GetParallelTasks())
		{
			if (ParallelInfo.TaskNode == TaskNode ||
				(TaskNode->IsInstanced() && ParallelInfo.TaskNode && ParallelInfo.TaskNode->GetExecutionIndex() == ExecutionIndex))
			{
				Status = ParallelInfo.Status;
				break;
			}
		}

		// 如果不是并行任务
		if (Status == EBTTaskStatus::Inactive)
		{
			// 如果任务节点是活动节点
			if (InstanceInfo.ActiveNode == TaskNode ||
				(TaskNode->IsInstanced() && InstanceInfo.ActiveNode && InstanceInfo.ActiveNode->GetExecutionIndex() == ExecutionIndex))
			{
				Status =
					(InstanceInfo.ActiveNodeType == EBTActiveNode::ActiveTask) ? EBTTaskStatus::Active :
					(InstanceInfo.ActiveNodeType == EBTActiveNode::AbortingTask) ? EBTTaskStatus::Aborting :
					EBTTaskStatus::Inactive;
			}
		}
	}

	return Status;
}

// 请求在指定分支中注销aux节点
void UBehaviorTreeComponent::RequestUnregisterAuxNodesInBranch(const UBTCompositeNode* Node)
{
	// 找到树实例索引
	const int32 InstanceIdx = FindInstanceContainingNode(Node);
	if (InstanceIdx != INDEX_NONE)
	{
		// 加入等待注册的辅助接点请求中
		PendingUnregisterAuxNodesRequests.Ranges.Emplace(
			FBTNodeIndex(InstanceIdx, Node->GetExecutionIndex()),
			FBTNodeIndex(InstanceIdx, Node->GetLastExecutionIndex()));

		ScheduleNextTick(0.0f);
	}
}

// 要求执行变更：装饰节点
void UBehaviorTreeComponent::RequestExecution(const UBTDecorator* RequestedBy)
{
	check(RequestedBy);
	// search range depends on decorator's FlowAbortMode:
	//
	// - LowerPri: try entering branch = search only nodes under decorator
	//
	// - Self: leave execution = from node under decorator to end of tree
	//
	// - Both: check if active node is within inner child nodes and choose Self or LowerPri
	//
	//搜索范围取决于装饰器的FlowAbortMode：
	//-LowerPri：尝试进入branch =仅搜索装饰器下的节点
	//-Self：离开执行=从装饰器下的节点到树的末端
	//-Both：检查活动节点是否在内部子节点内，然后选择Self或LowerPrime 

	EBTFlowAbortMode::Type AbortMode = RequestedBy->GetFlowAbortMode();
	if (AbortMode == EBTFlowAbortMode::None)
	{
		return;
	}

	const int32 InstanceIdx = FindInstanceContainingNode(RequestedBy->GetParentNode());
	if (InstanceIdx == INDEX_NONE)
	{
		return;
	}

#if ENABLE_VISUAL_LOG || DO_ENSURE
	const FBehaviorTreeInstance& ActiveInstance = InstanceStack.Last();
	if (ActiveInstance.ActiveNodeType == EBTActiveNode::ActiveTask)
	{
		EBTNodeRelativePriority RelativePriority = CalculateRelativePriority(RequestedBy, ActiveInstance.ActiveNode);

		if (RelativePriority < EBTNodeRelativePriority::Same)
		{
			const FString ErrorMsg(FString::Printf(TEXT("%s: decorator %s requesting restart has lower priority than Current Task %s"),
				ANSI_TO_TCHAR(__FUNCTION__),
				*UBehaviorTreeTypes::DescribeNodeHelper(RequestedBy),
				*UBehaviorTreeTypes::DescribeNodeHelper(ActiveInstance.ActiveNode)));

			UE_VLOG(GetOwner(), LogBehaviorTree, Error, TEXT("%s"), *ErrorMsg);
			ensureMsgf(false, TEXT("%s"), *ErrorMsg);
		}
	}
#endif // ENABLE_VISUAL_LOG || DO_ENSURE

	if (AbortMode == EBTFlowAbortMode::Both)
	{
		// 是否是执行的子节点
		const bool bIsExecutingChildNodes = IsExecutingBranch(RequestedBy, RequestedBy->GetChildIndex());
		AbortMode = bIsExecutingChildNodes ? EBTFlowAbortMode::Self : EBTFlowAbortMode::LowerPriority;
	}

	EBTNodeResult::Type ContinueResult = (AbortMode == EBTFlowAbortMode::Self) ? EBTNodeResult::Failed : EBTNodeResult::Aborted;
	// 回到父节点执行
	RequestExecution(RequestedBy->GetParentNode(), InstanceIdx, RequestedBy, RequestedBy->GetChildIndex(), ContinueResult);
}

// 返回NodeA对于NodeB的相对优先级
EBTNodeRelativePriority UBehaviorTreeComponent::CalculateRelativePriority(const UBTNode* NodeA, const UBTNode* NodeB) const
{
	EBTNodeRelativePriority RelativePriority = EBTNodeRelativePriority::Same;

	if (NodeA != NodeB)
	{
		// 先找到对应的树实例索引
		const int32 InstanceIndexA = FindInstanceContainingNode(NodeA);
		const int32 InstanceIndexB = FindInstanceContainingNode(NodeB);
		// 同一棵树
		if (InstanceIndexA == InstanceIndexB)
		{
			RelativePriority = NodeA->GetExecutionIndex() < NodeB->GetExecutionIndex() ? EBTNodeRelativePriority::Higher : EBTNodeRelativePriority::Lower;
		}
		else
		{
			// 如果树索引都合法的，索引小的优先级高
			RelativePriority = (InstanceIndexA != INDEX_NONE && InstanceIndexB != INDEX_NONE) ? (InstanceIndexA < InstanceIndexB ? EBTNodeRelativePriority::Higher : EBTNodeRelativePriority::Lower)
				: (InstanceIndexA != INDEX_NONE ? EBTNodeRelativePriority::Higher : EBTNodeRelativePriority::Lower);
		}
	}

	return RelativePriority;
}

// 要求执行变更：任务节点
void UBehaviorTreeComponent::RequestExecution(EBTNodeResult::Type LastResult)
{
	// task helpers can't continue with InProgress or Aborted result, it should be handled 
	// either by decorator helper or regular RequestExecution() (6 param version)

	if (LastResult != EBTNodeResult::Aborted && LastResult != EBTNodeResult::InProgress && InstanceStack.IsValidIndex(ActiveInstanceIdx))
	{
		const FBehaviorTreeInstance& ActiveInstance = InstanceStack[ActiveInstanceIdx];
		// 得到执行节点的父节点
		UBTCompositeNode* ExecuteParent = (ActiveInstance.ActiveNode == NULL) ? ActiveInstance.RootNode :
			(ActiveInstance.ActiveNodeType == EBTActiveNode::Composite) ? (UBTCompositeNode*)ActiveInstance.ActiveNode :
			ActiveInstance.ActiveNode->GetParentNode();

		// 执行父节点
		RequestExecution(ExecuteParent, InstanceStack.Num() - 1,
			ActiveInstance.ActiveNode ? ActiveInstance.ActiveNode : ActiveInstance.RootNode, -1,
			LastResult, false);
	}
}

// 查找共同的祖先
static void FindCommonParent(const TArray<FBehaviorTreeInstance>& Instances, const TArray<FBehaviorTreeInstanceId>& KnownInstances,
							 UBTCompositeNode* InNodeA, uint16 InstanceIdxA,
							 UBTCompositeNode* InNodeB, uint16 InstanceIdxB,
							 UBTCompositeNode*& CommonParentNode, uint16& CommonInstanceIdx)
{
	// find two nodes in the same instance (choose lower index = closer to root)
	// 在同一个实例上的两个节点（选择更小的索引，相当于更靠近根部）
	CommonInstanceIdx = (InstanceIdxA <= InstanceIdxB) ? InstanceIdxA : InstanceIdxB;

	// 靠近根的那个节点取父节点
	UBTCompositeNode* NodeA = (CommonInstanceIdx == InstanceIdxA) ? InNodeA : Instances[CommonInstanceIdx].ActiveNode->GetParentNode();
	UBTCompositeNode* NodeB = (CommonInstanceIdx == InstanceIdxB) ? InNodeB : Instances[CommonInstanceIdx].ActiveNode->GetParentNode();

	// special case: node was taken from CommonInstanceIdx, but it had ActiveNode set to root (no parent)
	// 特殊情况：节点取自CommonInstanceIdx，但其ActiveNode设置为root（无父节点） 
	if (!NodeA && CommonInstanceIdx != InstanceIdxA)
	{
		NodeA = Instances[CommonInstanceIdx].RootNode;
	}
	if (!NodeB && CommonInstanceIdx != InstanceIdxB)
	{
		NodeB = Instances[CommonInstanceIdx].RootNode;
	}

	// if one of nodes is still empty, we have serious problem with execution flow - crash and log details
	// 如果节点之一仍然为空，则我们的执行流程存在严重问题-崩溃和日志详细信息 
	if (!NodeA || !NodeB)
	{
		FString AssetAName = Instances.IsValidIndex(InstanceIdxA) && KnownInstances.IsValidIndex(Instances[InstanceIdxA].InstanceIdIndex) ? GetNameSafe(KnownInstances[Instances[InstanceIdxA].InstanceIdIndex].TreeAsset) : TEXT("unknown");
		FString AssetBName = Instances.IsValidIndex(InstanceIdxB) && KnownInstances.IsValidIndex(Instances[InstanceIdxB].InstanceIdIndex) ? GetNameSafe(KnownInstances[Instances[InstanceIdxB].InstanceIdIndex].TreeAsset) : TEXT("unknown");
		FString AssetCName = Instances.IsValidIndex(CommonInstanceIdx) && KnownInstances.IsValidIndex(Instances[CommonInstanceIdx].InstanceIdIndex) ? GetNameSafe(KnownInstances[Instances[CommonInstanceIdx].InstanceIdIndex].TreeAsset) : TEXT("unknown");

		UE_LOG(LogBehaviorTree, Fatal, TEXT("Fatal error in FindCommonParent() call.\nInNodeA: %s, InstanceIdxA: %d (%s), NodeA: %s\nInNodeB: %s, InstanceIdxB: %d (%s), NodeB: %s\nCommonInstanceIdx: %d (%s), ActiveNode: %s%s"),
			*UBehaviorTreeTypes::DescribeNodeHelper(InNodeA), InstanceIdxA, *AssetAName, *UBehaviorTreeTypes::DescribeNodeHelper(NodeA),
			*UBehaviorTreeTypes::DescribeNodeHelper(InNodeB), InstanceIdxB, *AssetBName, *UBehaviorTreeTypes::DescribeNodeHelper(NodeB),
			CommonInstanceIdx, *AssetCName, *UBehaviorTreeTypes::DescribeNodeHelper(Instances[CommonInstanceIdx].ActiveNode),
			(Instances[CommonInstanceIdx].ActiveNode == Instances[CommonInstanceIdx].RootNode) ? TEXT(" (root)") : TEXT(""));

		return;
	}

	// find common parent of two nodes
	// 查找两个节点的共同祖先
	int32 NodeADepth = NodeA->GetTreeDepth();
	int32 NodeBDepth = NodeB->GetTreeDepth();

	// 先往上遍历到同一深度
	while (NodeADepth > NodeBDepth)
	{
		NodeA = NodeA->GetParentNode();
		NodeADepth = NodeA->GetTreeDepth();
	}

	while (NodeBDepth > NodeADepth)
	{
		NodeB = NodeB->GetParentNode();
		NodeBDepth = NodeB->GetTreeDepth();
	}

	// 节点同时往上遍历
	while (NodeA != NodeB)
	{
		NodeA = NodeA->GetParentNode();
		NodeB = NodeB->GetParentNode();
	}

	CommonParentNode = NodeA;
}

// 在下一个tick安排执行流更新
void UBehaviorTreeComponent::ScheduleExecutionUpdate()
{
	ScheduleNextTick(0.0f);
	bRequestedFlowUpdate = true;
}

// 要求执行变更
void UBehaviorTreeComponent::RequestExecution(UBTCompositeNode* RequestedOn, int32 InstanceIdx, const UBTNode* RequestedBy,
											  int32 RequestedByChildIndex, EBTNodeResult::Type ContinueWithResult, bool bStoreForDebugger)
{
	SCOPE_CYCLE_COUNTER(STAT_AI_BehaviorTree_SearchTime);
#if !UE_BUILD_SHIPPING // Disable in shipping builds
	// Code for timing BT Search
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(BehaviorTreeSearch);

	FScopedSwitchedCountedDurationTimer ScopedSwitchedCountedDurationTimer(FrameSearchTime, NumSearchTimeCalls, CVarBTRecordFrameSearchTimes.GetValueOnGameThread() != 0);
#endif

	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Execution request by %s (result: %s)"),
		*UBehaviorTreeTypes::DescribeNodeHelper(RequestedBy),
		*UBehaviorTreeTypes::DescribeNodeResult(ContinueWithResult));

	// 检查行为树的状态
	if (!bIsRunning || !InstanceStack.IsValidIndex(ActiveInstanceIdx) || (GetOwner() && GetOwner()->IsPendingKillPending()))
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: tree is not running"));
		return;
	}

	const bool bOutOfNodesPending = PendingExecution.IsSet() && PendingExecution.bOutOfNodes;
	if (bOutOfNodesPending)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: tree ran out of nodes on previous restart and needs to process it first"));
		return;
	}

	const bool bSwitchToHigherPriority = (ContinueWithResult == EBTNodeResult::Aborted);
	const bool bAlreadyHasRequest = (ExecutionRequest.ExecuteNode != NULL);
	const UBTNode* DebuggerNode = bStoreForDebugger ? RequestedBy : NULL;

	FBTNodeIndex ExecutionIdx;
	ExecutionIdx.InstanceIndex = InstanceIdx;
	ExecutionIdx.ExecutionIndex = RequestedBy->GetExecutionIndex();
	uint16 LastExecutionIndex = MAX_uint16;

	// make sure that the request is not coming from a node that has pending unregistration since it won't be accessible anymore
	// 确保请求不是从未注册的节点来的，因为其已经不能在访问
	for (const FBTNodeIndexRange& Range : PendingUnregisterAuxNodesRequests.Ranges)
	{
		if (Range.Contains(ExecutionIdx))
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: request by %s that is in pending unregister aux nodes range %s"), *ExecutionIdx.Describe(), *Range.Describe());
			return;
		}
	}

	if (bSwitchToHigherPriority && RequestedByChildIndex >= 0)
	{
		ExecutionIdx.ExecutionIndex = RequestedOn->GetChildExecutionIndex(RequestedByChildIndex, EBTChildIndex::FirstNode);
		
		// first index outside allowed range		
		LastExecutionIndex = RequestedOn->GetChildExecutionIndex(RequestedByChildIndex + 1, EBTChildIndex::FirstNode);
	}

	// 搜索结尾
	const FBTNodeIndex SearchEnd(InstanceIdx, LastExecutionIndex);

	// check if it's more important than currently requested
	// 检查它是否比当前的请求更加重要
	if (bAlreadyHasRequest && ExecutionRequest.SearchStart.TakesPriorityOver(ExecutionIdx))
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: already has request with higher priority"));
		StoreDebuggerRestart(DebuggerNode, InstanceIdx, true);

		// make sure to update end of search range
		// 切换到更高的优先级
		if (bSwitchToHigherPriority)
		{
			// 替换SearchEnd，进行拓展
			if (ExecutionRequest.SearchEnd.IsSet() && ExecutionRequest.SearchEnd.TakesPriorityOver(SearchEnd))
			{
				UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> expanding end of search range!"));
				ExecutionRequest.SearchEnd = SearchEnd;
			}
		}
		else
		{
			// 移除SearchEnd的限制
			if (ExecutionRequest.SearchEnd.IsSet())
			{
				UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> removing limit from end of search range!"));
				ExecutionRequest.SearchEnd = FBTNodeIndex();
			}
		}

		return;
	}

    // Not only checking against deactivated branch upon applying search data or while aborting task, 
    // but also while waiting after a latent task to abort
	// 不仅在应用搜索数据时或在中止任务时检查停用的分支，还在等待潜在任务中止后等待 
	if (SearchData.bFilterOutRequestFromDeactivatedBranch || bWaitingForAbortingTasks)
	{
		// request on same node or with higher priority doesn't require additional checks
		// 在同一节点上或具有更高优先级的请求不需要其他检查 
		if (SearchData.SearchRootNode != ExecutionIdx && SearchData.SearchRootNode.TakesPriorityOver(ExecutionIdx))
		{
			if (ExecutionIdx == SearchData.DeactivatedBranchStart ||
				(SearchData.DeactivatedBranchStart.TakesPriorityOver(ExecutionIdx) && ExecutionIdx.TakesPriorityOver(SearchData.DeactivatedBranchEnd)))
			{
				UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: node index %s in a deactivated branch [%s..%s[ (applying search data for %s)"),
					*ExecutionIdx.Describe(), *SearchData.DeactivatedBranchStart.Describe(), *SearchData.DeactivatedBranchEnd.Describe(), *SearchData.SearchRootNode.Describe());
				StoreDebuggerRestart(DebuggerNode, InstanceIdx, false);
				return;
			}
		}
	}

	// when it's aborting and moving to higher priority node:
	// 当它终止并且转移到一个更高优先级的节点
	if (bSwitchToHigherPriority)
	{
		// check if decorators allow execution on requesting link
		// unless it's branch restart (abort result within current branch), when it can't be skipped because branch can be no longer valid
		// 检查装饰器是否允许在请求的链接上执行，除非分支重新启动（当前分支内的中止结果），否则由于分支不再有效而无法跳过装饰 
		const bool bShouldCheckDecorators = (RequestedByChildIndex >= 0) && !IsExecutingBranch(RequestedBy, RequestedByChildIndex);
		// 如果不需要检查装饰节点
		const bool bCanExecute = !bShouldCheckDecorators || RequestedOn->DoDecoratorsAllowExecution(*this, InstanceIdx, RequestedByChildIndex);
		if (!bCanExecute)
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: decorators are not allowing execution"));
			StoreDebuggerRestart(DebuggerNode, InstanceIdx, false);
			return;
		}

		// update common parent: requesting node with prev common/active node
		// 更新公共父节点：具有上一个公共/活动节点的请求节点
		UBTCompositeNode* CurrentNode = ExecutionRequest.ExecuteNode;
		uint16 CurrentInstanceIdx = ExecutionRequest.ExecuteInstanceIdx;
		// 当前执行的节点为空，设置当前的执行节点和当前的树实例索引
		if (ExecutionRequest.ExecuteNode == NULL)
		{
			FBehaviorTreeInstance& ActiveInstance = InstanceStack[ActiveInstanceIdx];
			CurrentNode = (ActiveInstance.ActiveNode == NULL) ? ActiveInstance.RootNode :
				(ActiveInstance.ActiveNodeType == EBTActiveNode::Composite) ? (UBTCompositeNode*)ActiveInstance.ActiveNode :
				ActiveInstance.ActiveNode->GetParentNode();

			CurrentInstanceIdx = ActiveInstanceIdx;
		}

		// 当前节点和请求执行的节点不一致
		if (ExecutionRequest.ExecuteNode != RequestedOn)
		{
			UBTCompositeNode* CommonParent = NULL;
			uint16 CommonInstanceIdx = MAX_uint16;
			// 搜索共同的父节点
			FindCommonParent(InstanceStack, KnownInstances, RequestedOn, InstanceIdx, CurrentNode, CurrentInstanceIdx, CommonParent, CommonInstanceIdx);

			// check decorators between common parent and restart parent
			// 检查共同祖先和重启的父节点之间的装饰器
			int32 ItInstanceIdx = InstanceIdx;
			for (UBTCompositeNode* It = RequestedOn; It && It != CommonParent;)
			{
				UBTCompositeNode* ParentNode = It->GetParentNode();
				int32 ChildIdx = INDEX_NONE;

				if (ParentNode == nullptr)
				{
					// move up the tree stack
					// 树堆栈往上移动一格
					if (ItInstanceIdx > 0)
					{
						ItInstanceIdx--;
						UBTNode* SubtreeTaskNode = InstanceStack[ItInstanceIdx].ActiveNode;
						ParentNode = SubtreeTaskNode->GetParentNode();
						ChildIdx = ParentNode->GetChildIndex(*SubtreeTaskNode);
					}
					else
					{
						// something went wrong...
						break;
					}
				}
				else
				{
					// 得到节点在父节点的子节点索引
					ChildIdx = ParentNode->GetChildIndex(*It);
				}
				// 装饰节点是否允许执行
				const bool bCanExecuteTest = ParentNode->DoDecoratorsAllowExecution(*this, ItInstanceIdx, ChildIdx);
				if (!bCanExecuteTest)
				{
					UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: decorators are not allowing execution"));
					StoreDebuggerRestart(DebuggerNode, InstanceIdx, false);
					return;
				}

				It = ParentNode;
			}

			ExecutionRequest.ExecuteNode = CommonParent;
			ExecutionRequest.ExecuteInstanceIdx = CommonInstanceIdx;
		}
	}
	else
	{
		// check if decorators allow execution on requesting link (only when restart comes from composite decorator)
		// 检查装饰器是否允许在请求链接上执行（仅当重新启动来自复合装饰器时） 
		const bool bShouldCheckDecorators = RequestedOn->Children.IsValidIndex(RequestedByChildIndex) &&
			(RequestedOn->Children[RequestedByChildIndex].DecoratorOps.Num() > 0) &&
			RequestedBy->IsA(UBTDecorator::StaticClass());

		// 装饰器是否允许执行
		const bool bCanExecute = bShouldCheckDecorators && RequestedOn->DoDecoratorsAllowExecution(*this, InstanceIdx, RequestedByChildIndex);
		if (bCanExecute)
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> skip: decorators are still allowing execution"));
			StoreDebuggerRestart(DebuggerNode, InstanceIdx, false);
			return;
		}

		ExecutionRequest.ExecuteNode = RequestedOn;
		ExecutionRequest.ExecuteInstanceIdx = InstanceIdx;
	}

	// store it
	StoreDebuggerRestart(DebuggerNode, InstanceIdx, true);

	// search end can be set only when switching to high priority
	// or previous request was limited and current limit is wider
	// 仅当切换到高优先级或先前的请求受到限制并且当前限制更宽时，才可以设置搜索结束
	if ((!bAlreadyHasRequest && bSwitchToHigherPriority) ||
		(ExecutionRequest.SearchEnd.IsSet() && ExecutionRequest.SearchEnd.TakesPriorityOver(SearchEnd)))
	{
		UE_CVLOG(bAlreadyHasRequest, GetOwner(), LogBehaviorTree, Log, TEXT("%s"), (SearchEnd.ExecutionIndex < MAX_uint16) ? TEXT("> expanding end of search range!") : TEXT("> removing limit from end of search range!"));
		ExecutionRequest.SearchEnd = SearchEnd;
	}

	ExecutionRequest.SearchStart = ExecutionIdx;
	ExecutionRequest.ContinueWithResult = ContinueWithResult;
	ExecutionRequest.bTryNextChild = !bSwitchToHigherPriority;
	ExecutionRequest.bIsRestart = (RequestedBy != GetActiveNode());
	PendingExecution.Lock();
	
	// break out of current search if new request is more important than currently processed one
	// no point in starting new task just to abandon it in next tick
	// 如果新请求比当前处理的请求更重要，则中断当前搜索，开始新任务只是在下一个tick中放弃就没有意义了 
	if (SearchData.bSearchInProgress)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> aborting current task search!"));
		SearchData.bPostponeSearch = true;
	}

	// latent task abort:
	// - don't search, just accumulate requests and run them when abort is done
	// - rollback changes from search that caused abort to ensure proper state of tree
	//潜在任务中止：
	//-不搜索，仅累积请求并在中止完成后运行它们
	//-搜索的回滚更改导致中止以确保树的正确状态 
	const bool bIsActiveNodeAborting = InstanceStack.Num() && InstanceStack.Last().ActiveNodeType == EBTActiveNode::AbortingTask;
	const bool bInvalidateCurrentSearch = bWaitingForAbortingTasks || bIsActiveNodeAborting;
	const bool bScheduleNewSearch = !bWaitingForAbortingTasks;

	if (bInvalidateCurrentSearch)
	{
        // We are aborting the current search, but in the case we were searching to a next child, we cannot look for only higher priority as sub decorator might still fail
		// Previous search might have been a different range, so just open it up to cover all cases
		// 我们正在中止当前的搜索，但是在我们搜索下一个孩子的情况下，我们不能只寻找更高的优先级，
		// 因为子装饰器仍然可能失败先前的搜索可能是一个不同的范围，因此只需打开它来覆盖 所有情况 
		if (ExecutionRequest.SearchEnd.IsSet())
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("> removing limit from end of search range because of request during task abortion!"));
			ExecutionRequest.SearchEnd = FBTNodeIndex();
		}
		RollbackSearchChanges();
	}
	
	if (bScheduleNewSearch)
	{
		ScheduleExecutionUpdate();
	}
}

// 应用特定列表中的更新 
void UBehaviorTreeComponent::ApplySearchUpdates(const TArray<FBehaviorTreeSearchUpdate>& UpdateList, int32 NewNodeExecutionIndex, bool bPostUpdate)
{
	// 遍历所有的更新列表
	for (int32 Index = 0; Index < UpdateList.Num(); Index++)
	{
		const FBehaviorTreeSearchUpdate& UpdateInfo = UpdateList[Index];
		if (!InstanceStack.IsValidIndex(UpdateInfo.InstanceIndex))
		{
			continue;
		}

		// 找到更新的树实例
		FBehaviorTreeInstance& UpdateInstance = InstanceStack[UpdateInfo.InstanceIndex];
		int32 ParallelTaskIdx = INDEX_NONE;
		bool bIsComponentActive = false;

		// 辅助节点
		if (UpdateInfo.AuxNode)
		{
			// 是否是激活的辅助节点
			bIsComponentActive = UpdateInstance.GetActiveAuxNodes().Contains(UpdateInfo.AuxNode);
		}
		// 任务节点
		else if (UpdateInfo.TaskNode)
		{
			// 并行任务索引
			ParallelTaskIdx = UpdateInstance.GetParallelTasks().IndexOfByKey(UpdateInfo.TaskNode);
			// 是否是激活的辅助节点
			bIsComponentActive = (ParallelTaskIdx != INDEX_NONE && UpdateInstance.GetParallelTasks()[ParallelTaskIdx].Status == EBTTaskStatus::Active);
		}

		// 得到更新节点
		const UBTNode* UpdateNode = UpdateInfo.AuxNode ? (const UBTNode*)UpdateInfo.AuxNode : (const UBTNode*)UpdateInfo.TaskNode;
		checkSlow(UpdateNode);

		// 如果是删除一个非活动节点，或者增加一个活动节点，或者是否延迟更新的状态不对，都跳过
		if ((UpdateInfo.Mode == EBTNodeUpdateMode::Remove && !bIsComponentActive) ||
			(UpdateInfo.Mode == EBTNodeUpdateMode::Add && (bIsComponentActive || UpdateNode->GetExecutionIndex() > NewNodeExecutionIndex)) ||
			(UpdateInfo.bPostUpdate != bPostUpdate))
		{
			continue;
		}

		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Update: %s for %s: %s"),
			*UBehaviorTreeTypes::DescribeNodeUpdateMode(UpdateInfo.Mode),
			UpdateInfo.AuxNode ? TEXT("auxiliary node") : TEXT("parallel's main task"),
			*UBehaviorTreeTypes::DescribeNodeHelper(UpdateNode));

		if (UpdateInfo.AuxNode)
		{
			// special case: service node at root of top most subtree - don't remove/re-add them when tree is in looping mode
			// don't bother with decorators parent == root means that they are on child branches
			// 特殊情况：服务节点位于最顶层子树的根目录下-当树处于循环模式时，请勿删除/重新添加它们
			// 不要打扰装饰器parent == root表示它们在子分支上 
			if (bLoopExecution && UpdateInfo.AuxNode->GetMyNode() == InstanceStack[0].RootNode &&
				UpdateInfo.AuxNode->IsA(UBTService::StaticClass()))
			{
				if (UpdateInfo.Mode == EBTNodeUpdateMode::Remove ||
					InstanceStack[0].GetActiveAuxNodes().Contains(UpdateInfo.AuxNode))
				{
					UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("> skip [looped execution]"));
					continue;
				}
			}

			uint8* NodeMemory = (uint8*)UpdateNode->GetNodeMemory<uint8>(UpdateInstance);
			// 更新模式来增加或者删除节点，并且调用对应的通知函数
			if (UpdateInfo.Mode == EBTNodeUpdateMode::Remove)
			{
				UpdateInstance.RemoveFromActiveAuxNodes(UpdateInfo.AuxNode);
				UpdateInfo.AuxNode->WrappedOnCeaseRelevant(*this, NodeMemory);
			}
			else
			{
				UpdateInstance.AddToActiveAuxNodes(UpdateInfo.AuxNode);
				UpdateInfo.AuxNode->WrappedOnBecomeRelevant(*this, NodeMemory);
			}
		}
		else if (UpdateInfo.TaskNode)
		{
			// 删除任务节点
			if (UpdateInfo.Mode == EBTNodeUpdateMode::Remove)
			{
				// remove all message observers from node to abort to avoid calling OnTaskFinished from AbortTask
				// 从节点删除所有消息观察者以中止以避免从AbortTask调用OnTaskFinished 
				UnregisterMessageObserversFrom(UpdateInfo.TaskNode);

				uint8* NodeMemory = (uint8*)UpdateNode->GetNodeMemory<uint8>(UpdateInstance);
				// 通知节点终止任务
				EBTNodeResult::Type NodeResult = UpdateInfo.TaskNode->WrappedAbortTask(*this, NodeMemory);

				UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Parallel task aborted: %s (%s)"),
					*UBehaviorTreeTypes::DescribeNodeHelper(UpdateInfo.TaskNode),
					(NodeResult == EBTNodeResult::InProgress) ? TEXT("in progress") : TEXT("instant"));

				// check if task node is still valid, could've received LatentAbortFinished during AbortTask call
				// 检查任务节点是否仍然有效，是否可以在AbortTask调用期间收到LatentAbortFinished 
				const bool bStillValid = InstanceStack.IsValidIndex(UpdateInfo.InstanceIndex) &&
					InstanceStack[UpdateInfo.InstanceIndex].GetParallelTasks().IsValidIndex(ParallelTaskIdx) &&
					InstanceStack[UpdateInfo.InstanceIndex].GetParallelTasks()[ParallelTaskIdx] == UpdateInfo.TaskNode;
				
				// 仍然有效
				if (bStillValid)
				{
					// mark as pending abort
					// 执行结果是处理过程中
					if (NodeResult == EBTNodeResult::InProgress)
					{
						// 将并行任务挂起中止
						UpdateInstance.MarkParallelTaskAsAbortingAt(ParallelTaskIdx);
						bWaitingForAbortingTasks = true;
					}
					// 调用任务完成
					OnTaskFinished(UpdateInfo.TaskNode, NodeResult);
				}
			}
			else
			{
				UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("Parallel task: %s added to active list"),
					*UBehaviorTreeTypes::DescribeNodeHelper(UpdateInfo.TaskNode));
				// 增加并行任务
				UpdateInstance.AddToParallelTasks(FBehaviorTreeParallelTask(UpdateInfo.TaskNode, EBTTaskStatus::Active));
			}
		}
	}
}

void UBehaviorTreeComponent::ApplySearchData(UBTNode* NewActiveNode)
{
	// search is finalized, can't rollback anymore at this point
	// 搜索结束，再也不能回滚到该点了
	SearchData.RollbackInstanceIdx = INDEX_NONE;
	SearchData.RollbackDeactivatedBranchStart = FBTNodeIndex();
	SearchData.RollbackDeactivatedBranchEnd = FBTNodeIndex();

	// send all deactivation notifies for bookkeeping
	// 给发送所有的停用通知
	for (int32 Idx = 0; Idx < SearchData.PendingNotifies.Num(); Idx++)
	{
		const FBehaviorTreeSearchUpdateNotify& NotifyInfo = SearchData.PendingNotifies[Idx];
		if (InstanceStack.IsValidIndex(NotifyInfo.InstanceIndex))
		{
			InstanceStack[NotifyInfo.InstanceIndex].DeactivationNotify.ExecuteIfBound(*this, NotifyInfo.NodeResult);
		}	
	}

	// apply changes to aux nodes and parallel tasks
	// 应用改变到辅助接点和并行任务
	const int32 NewNodeExecutionIndex = NewActiveNode ? NewActiveNode->GetExecutionIndex() : 0;

	// 跳过来自停用分支中节点的执行请求
	SearchData.bFilterOutRequestFromDeactivatedBranch = true;

	// 应用更新
	ApplySearchUpdates(SearchData.PendingUpdates, NewNodeExecutionIndex);
	ApplySearchUpdates(SearchData.PendingUpdates, NewNodeExecutionIndex, true);
	
	// 接受来自停用分支中节点的执行请求
	SearchData.bFilterOutRequestFromDeactivatedBranch = false;

	// tick newly added aux nodes to compensate for tick-search order changes
	// tick新添加的辅助节点来补偿tick搜索顺序更改
	UWorld* MyWorld = GetWorld();
	const float CurrentFrameDeltaSeconds = MyWorld ? MyWorld->GetDeltaSeconds() : 0.0f;

	// 遍历等待的更新
	for (int32 Idx = 0; Idx < SearchData.PendingUpdates.Num(); Idx++)
	{
		const FBehaviorTreeSearchUpdate& UpdateInfo = SearchData.PendingUpdates[Idx];
		// 如果是新增加的
		if (UpdateInfo.Mode == EBTNodeUpdateMode::Add && UpdateInfo.AuxNode && InstanceStack.IsValidIndex(UpdateInfo.InstanceIndex))
		{
			FBehaviorTreeInstance& InstanceInfo = InstanceStack[UpdateInfo.InstanceIndex];
			uint8* NodeMemory = UpdateInfo.AuxNode->GetNodeMemory<uint8>(InstanceInfo);

            // We do not care about the next needed DeltaTime, it will be recalculated in the tick later.
			// 不关心下一次需要的DeltaTime，它将稍后的tick中重新计算
			float NextNeededDeltaTime = 0.0f;
			UpdateInfo.AuxNode->WrappedTickNode(*this, NodeMemory, CurrentFrameDeltaSeconds, NextNeededDeltaTime);
		}
	}

	// clear update list
	// nothing should be added during application or tick - all changes are supposed to go to ExecutionRequest accumulator first
	// 清空更新列表
	// 在应用程序或者tick期间任何东西都不要加入进去，所有的更改都应该先转到ExecutionRequest的累加器中去
	SearchData.PendingUpdates.Reset();
	SearchData.PendingNotifies.Reset();
	SearchData.DeactivatedBranchStart = FBTNodeIndex();
	SearchData.DeactivatedBranchEnd = FBTNodeIndex();
}

// 通过放弃搜索的方式来应用等待的节点更新要求
void UBehaviorTreeComponent::ApplyDiscardedSearch()
{
	// remove everything else
	// 删除所有的未应用的更新
	SearchData.PendingUpdates.Reset();

	// don't send deactivation notifies
	// 清空停用通知
	SearchData.PendingNotifies.Reset();
}

// 组件的tick
void UBehaviorTreeComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	// Warn if BT asked to be ticked the next frame and did not.
	// 如果BT要求在下一帧tick，但是没有就需要发出警告
	if (bTickedOnce && NextTickDeltaTime == 0.0f)
	{
		UWorld* MyWorld = GetWorld();
		if (MyWorld)
		{
			const float CurrentGameTime = MyWorld->GetTimeSeconds();
			const float CurrentDeltaTime = MyWorld->GetDeltaSeconds();
			// 当前游戏时间 - 要求下一帧tick的游戏时间 - 两帧之间的间隔时间 > 
			if (CurrentGameTime - LastRequestedDeltaTimeGameTime - CurrentDeltaTime > KINDA_SMALL_NUMBER)
			{
				UE_VLOG(GetOwner(), LogBehaviorTree, Error, TEXT("BT(%i) expected to be tick next frame, current deltatime(%f) and calculated deltatime(%f)."), GFrameCounter, CurrentDeltaTime, CurrentGameTime - LastRequestedDeltaTimeGameTime);
			}
		}
	}

	// Check if we really have reached the asked DeltaTime, 
	// If not then accumulate it and reschedule
	// 检查我们是否真的已经达到请求的增量时间
	// 如果没有达到就累计它并且重新安排
	NextTickDeltaTime -= DeltaTime;
	if (NextTickDeltaTime > 0.0f)
	{
		// The TickManager is using global time to calculate delta since last ticked time. When the value is big, we can get into float precision errors compare to our calculation.
		// TickManager正在使用全局时间来计算自上次tick时间以来的变化量。 当该值较大时，我们可以将浮点精度误差与我们的计算进行比较。 
		if (NextTickDeltaTime > KINDA_SMALL_NUMBER)
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Error, TEXT("BT(%i) did not need to be tick, ask deltatime of %fs got %fs with a diff of %fs."), GFrameCounter, NextTickDeltaTime + AccumulatedTickDeltaTime + DeltaTime, DeltaTime + AccumulatedTickDeltaTime, NextTickDeltaTime);
		}
		AccumulatedTickDeltaTime += DeltaTime;
		// 上面减少tick的时间
		ScheduleNextTick(NextTickDeltaTime);
		return;
	}
	// 计算tick成功的间隔时间
	DeltaTime += AccumulatedTickDeltaTime;
	// 累计时间置零
	AccumulatedTickDeltaTime = 0.0f;

	const bool bWasTickedOnce = bTickedOnce;
	bTickedOnce = true;

	// 是否有消息去处理
	bool bDoneSomething = MessagesToProcess.Num() > 0;
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	SCOPE_CYCLE_COUNTER(STAT_AI_Overall);
	SCOPE_CYCLE_COUNTER(STAT_AI_BehaviorTree_Tick);
#if CSV_PROFILER
	// Configurable CSV_SCOPED_TIMING_STAT_EXCLUSIVE(BehaviorTreeTick);
	FScopedCsvStatExclusive _ScopedCsvStatExclusive_BehaviorTreeTick(CSVTickStatName);
#endif

	check(this != nullptr && this->IsPendingKill() == false);
	float NextNeededDeltaTime = FLT_MAX;

	// process all auxiliary nodes unregister requests
	// 处理所有辅助接点的取消注册请求
	bDoneSomething |= ProcessPendingUnregister();

	// tick active auxiliary nodes (in execution order, before task)
	// do it before processing execution request to give BP driven logic chance to accumulate execution requests
	// newly added aux nodes are ticked as part of SearchData application
	// tick激活辅助节点（按照任务前的执行顺序）
	// 在处理执行请求之前执行此操作，以使BP驱动的逻辑有机会积累执行请求
	// 新添加的辅助节点作为SearchData应用程序的一部分去ticked
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		// tick所有激活的辅助节点
		FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		InstanceInfo.ExecuteOnEachAuxNode([&InstanceInfo, this, &bDoneSomething, DeltaTime, &NextNeededDeltaTime](const UBTAuxiliaryNode& AuxNode)
			{
				uint8* NodeMemory = AuxNode.GetNodeMemory<uint8>(InstanceInfo);
				SCOPE_CYCLE_UOBJECT(AuxNode, &AuxNode);
				bDoneSomething |= AuxNode.WrappedTickNode(*this, NodeMemory, DeltaTime, NextNeededDeltaTime);
			});
	}

	bool bActiveAuxiliaryNodeDTDirty = false;
	// 请求处理
	if (bRequestedFlowUpdate)
	{
		ProcessExecutionRequest();
		bDoneSomething = true;

        // Since hierarchy might changed in the ProcessExecutionRequest, we need to go through all the active auxiliary nodes again to fetch new next DeltaTime
		bActiveAuxiliaryNodeDTDirty = true;
		NextNeededDeltaTime = FLT_MAX;
	}

	//运行状态并且没有暂停
	if (InstanceStack.Num() > 0 && bIsRunning && !bIsPaused)
	{
		{
			FScopedBehaviorTreeLock ScopedLock(*this, FScopedBehaviorTreeLock::LockTick);

			// tick active parallel tasks (in execution order, before task)
			// tick所有激活的并行任务
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
			{
				FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
				InstanceInfo.ExecuteOnEachParallelTask([&InstanceInfo, &bDoneSomething, this, DeltaTime, &NextNeededDeltaTime](const FBehaviorTreeParallelTask& ParallelTaskInfo, const int32 Index)
					{
						const UBTTaskNode* ParallelTask = ParallelTaskInfo.TaskNode;
						SCOPE_CYCLE_UOBJECT(ParallelTask, ParallelTask);
						uint8* NodeMemory = ParallelTask->GetNodeMemory<uint8>(InstanceInfo);
						bDoneSomething |= ParallelTask->WrappedTickTask(*this, NodeMemory, DeltaTime, NextNeededDeltaTime);
					});
			}

			// tick active task
			// tick激活的任务节点
			if (InstanceStack.IsValidIndex(ActiveInstanceIdx))
			{
				FBehaviorTreeInstance& ActiveInstance = InstanceStack[ActiveInstanceIdx];
				if (ActiveInstance.ActiveNodeType == EBTActiveNode::ActiveTask ||
					ActiveInstance.ActiveNodeType == EBTActiveNode::AbortingTask)
				{
					UBTTaskNode* ActiveTask = (UBTTaskNode*)ActiveInstance.ActiveNode;
					uint8* NodeMemory = ActiveTask->GetNodeMemory<uint8>(ActiveInstance);
					SCOPE_CYCLE_UOBJECT(ActiveTask, ActiveTask);
					bDoneSomething |= ActiveTask->WrappedTickTask(*this, NodeMemory, DeltaTime, NextNeededDeltaTime);
				}
			}

			// tick aborting task from abandoned subtree
			// tick废弃子树的中止任务
			if (InstanceStack.IsValidIndex(ActiveInstanceIdx + 1))
			{
				FBehaviorTreeInstance& LastInstance = InstanceStack.Last();
				if (LastInstance.ActiveNodeType == EBTActiveNode::AbortingTask)
				{
					UBTTaskNode* ActiveTask = (UBTTaskNode*)LastInstance.ActiveNode;
					uint8* NodeMemory = ActiveTask->GetNodeMemory<uint8>(LastInstance);
					SCOPE_CYCLE_UOBJECT(ActiveTask, ActiveTask);
					bDoneSomething |= ActiveTask->WrappedTickTask(*this, NodeMemory, DeltaTime, NextNeededDeltaTime);
				}
			}
		}

		// 如果设置了，就在tick函数结尾处调用StopTree
		if (bDeferredStopTree)
		{
			StopTree(EBTStopMode::Safe);
			bDoneSomething = true;
		}
	}

	// Go through all active auxiliary nodes to calculate the next NeededDeltaTime if needed
	// 遍历所有激活的辅助接点，计算下一个需要的NeededDeltaTime
	if (bActiveAuxiliaryNodeDTDirty)
	{
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num() && NextNeededDeltaTime > 0.0f; InstanceIndex++)
		{
			FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
			for (const UBTAuxiliaryNode* AuxNode : InstanceInfo.GetActiveAuxNodes())
			{
				uint8* NodeMemory = AuxNode->GetNodeMemory<uint8>(InstanceInfo);
				const float NextNodeNeededDeltaTime = AuxNode->GetNextNeededDeltaTime(*this, NodeMemory);
				// 找到最小的时间
				if (NextNeededDeltaTime > NextNodeNeededDeltaTime)
				{
					NextNeededDeltaTime = NextNodeNeededDeltaTime;
				}
			}
		}
	}

	if (bWasTickedOnce && !bDoneSomething)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Error, TEXT("BT(%i) planned to do something but actually did not."), GFrameCounter);
	}
	// 安排下一次的tick时间
	ScheduleNextTick(NextNeededDeltaTime);

#if DO_ENSURE
	// Adding code to track an problem earlier that is happening by RequestExecution from a decorator that has lower priority.
	// The idea here is to try to rule out that the tick leaves the behavior tree is a bad state with lower priority decorators(AuxNodes).
	// 添加代码以更早地跟踪优先级较低的装饰器发出的RequestExecution发生的问题，
	// 此处的想法是尝试排除刻度线离开行为树是具有较低优先级装饰器（AuxNodes）的不良状态。
	static bool bWarnOnce = false;
	if (!bWarnOnce)
	{
		// 遍历所有的实例
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
		{
			const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
			if (!InstanceInfo.ActiveNode)
			{
				break;
			}

			// 遍历所有激活的辅助节点
			const uint16 ActiveExecutionIdx = InstanceInfo.ActiveNode->GetExecutionIndex();
			for (const UBTAuxiliaryNode* ActiveAuxNode : InstanceInfo.GetActiveAuxNodes())
			{
				// 找出比活动节点优先级低的节点
				if (ActiveAuxNode->GetExecutionIndex() >= ActiveExecutionIdx)
				{
					FString ErrorMsg(FString::Printf(TEXT("%s: leaving the tick of behavior tree with a lower priority active node %s, Current Tasks : "),
						ANSI_TO_TCHAR(__FUNCTION__),
						*UBehaviorTreeTypes::DescribeNodeHelper(ActiveAuxNode)));

					for (int32 ParentInstanceIndex = 0; ParentInstanceIndex <= InstanceIndex; ++ParentInstanceIndex)
					{
						ErrorMsg += *UBehaviorTreeTypes::DescribeNodeHelper(InstanceStack[ParentInstanceIndex].ActiveNode);
						ErrorMsg += TEXT("\\");
					}

					UE_VLOG(GetOwner(), LogBehaviorTree, Error, TEXT("%s"), *ErrorMsg);
					ensureMsgf(false, TEXT("%s"), *ErrorMsg);
					bWarnOnce = true;
					break;
				}
			}
		}
	}
#endif // DO_ENSURE
}

// 安排下一帧的时间，0.0f表示下一帧，FLT_MAX表示从来不
void UBehaviorTreeComponent::ScheduleNextTick(const float NextNeededDeltaTime)
{
	NextTickDeltaTime = NextNeededDeltaTime;
	if (bRequestedFlowUpdate)
	{
		NextTickDeltaTime = 0.0f;
	}

	UE_VLOG(GetOwner(), LogBehaviorTree, VeryVerbose, TEXT("BT(%i) schedule next tick %f, asked %f."), GFrameCounter, NextTickDeltaTime, NextNeededDeltaTime);
	// 如果NextTickDeltaTime设置成FLT_MAX，直接把ComponentTick禁用了
	if (NextTickDeltaTime == FLT_MAX)
	{
		if (IsComponentTickEnabled())
		{
			SetComponentTickEnabled(false);
		}
	}
	else
	{
		// 如果是禁用状态，就启用
		if (!IsComponentTickEnabled())
		{
			SetComponentTickEnabled(true);
		}
		// We need to force a small dt to tell the TickTaskManager we might not want to be tick every frame.
		// 我们需要强制一个小的dt告诉TickTaskManager我们可能不想在每一帧都tick。 
		const float FORCE_TICK_INTERVAL_DT = KINDA_SMALL_NUMBER;
		SetComponentTickIntervalAndCooldown(!bTickedOnce && NextTickDeltaTime < FORCE_TICK_INTERVAL_DT ? FORCE_TICK_INTERVAL_DT : NextTickDeltaTime);
	}
	UWorld* MyWorld = GetWorld();
	// 记录修改DeltaTime的游戏时间
	LastRequestedDeltaTimeGameTime = MyWorld ? MyWorld->GetTimeSeconds() : 0.0f;
}

// 处理执行流
void UBehaviorTreeComponent::ProcessExecutionRequest()
{
	bRequestedFlowUpdate = false;
	// 组件注册检查和激活实例索引是否合法检查
	if (!IsRegistered() || !InstanceStack.IsValidIndex(ActiveInstanceIdx))
	{
		// it shouldn't be called, component is no longer valid
		return;
	}

	// 是否已经暂停
	if (bIsPaused)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("Ignoring ProcessExecutionRequest call due to BTComponent still being paused"));
		return;
	}

	// 或者等待中止的任务
	if (bWaitingForAbortingTasks)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("Ignoring ProcessExecutionRequest call, aborting task must finish first"));
		return;
	}

	// 等待执行的请求
	if (PendingExecution.IsSet())
	{
		// 处理上一个搜索任务中待执行的请求
		ProcessPendingExecution();
		return;
	}

	bool bIsSearchValid = true;
	// 保存回滚的数据
	SearchData.RollbackInstanceIdx = ActiveInstanceIdx;
	SearchData.RollbackDeactivatedBranchStart = SearchData.DeactivatedBranchStart;
	SearchData.RollbackDeactivatedBranchEnd = SearchData.DeactivatedBranchEnd;

	// 节点结果
	EBTNodeResult::Type NodeResult = ExecutionRequest.ContinueWithResult;
	UBTTaskNode* NextTask = NULL;

	{
		SCOPE_CYCLE_COUNTER(STAT_AI_BehaviorTree_SearchTime);

#if !UE_BUILD_SHIPPING
		// Code for timing BT Search
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(BehaviorTreeSearch);

		FScopedSwitchedCountedDurationTimer ScopedSwitchedCountedDurationTimer(FrameSearchTime, NumSearchTimeCalls, CVarBTRecordFrameSearchTimes.GetValueOnGameThread() != 0);
#endif

		// copy current memory in case we need to rollback search
		// 将当前实例的内存拷贝到持久化内存，以防我们需要回滚搜索 
		CopyInstanceMemoryToPersistent();

		// deactivate up to ExecuteNode
		// 停用执行节点
		if (InstanceStack[ActiveInstanceIdx].ActiveNode != ExecutionRequest.ExecuteNode)
		{
			int32 LastDeactivatedChildIndex = INDEX_NONE;
			// 停用激活节点到执行节点的所有中间节点
			const bool bDeactivated = DeactivateUpTo(ExecutionRequest.ExecuteNode, ExecutionRequest.ExecuteInstanceIdx, NodeResult, LastDeactivatedChildIndex);
			if (!bDeactivated)
			{
				// error occurred and tree will restart, all pending deactivation notifies will be lost
				// this is should happen

				BT_SEARCHLOG(SearchData, Error, TEXT("Unable to deactivate up to %s. Active node is %s. All pending updates will be lost!"), 
					*UBehaviorTreeTypes::DescribeNodeHelper(ExecutionRequest.ExecuteNode), 
					*UBehaviorTreeTypes::DescribeNodeHelper(InstanceStack[ActiveInstanceIdx].ActiveNode));
				SearchData.PendingUpdates.Reset();

				return;
			}
			else if (LastDeactivatedChildIndex != INDEX_NONE)
			{
				// Calculating/expanding the deactivated branch for filtering execution request while applying changes.
				// 计算/扩展停用的分支，以在应用更改时过滤执行请求。
				FBTNodeIndex NewDeactivatedBranchStart(ExecutionRequest.ExecuteInstanceIdx, ExecutionRequest.ExecuteNode->GetChildExecutionIndex(LastDeactivatedChildIndex, EBTChildIndex::FirstNode));
				FBTNodeIndex NewDeactivatedBranchEnd(ExecutionRequest.ExecuteInstanceIdx, ExecutionRequest.ExecuteNode->GetChildExecutionIndex(LastDeactivatedChildIndex + 1, EBTChildIndex::FirstNode));

				// 取优先级高的
				if (NewDeactivatedBranchStart.TakesPriorityOver(SearchData.DeactivatedBranchStart))
				{
					SearchData.DeactivatedBranchStart = NewDeactivatedBranchStart;
				}
				ensureMsgf( !SearchData.DeactivatedBranchEnd.IsSet() || SearchData.DeactivatedBranchEnd == NewDeactivatedBranchEnd, TEXT("There should not be a case of an exiting dead branch with a different end index (Previous end:%s, New end:%s"), *SearchData.DeactivatedBranchEnd.Describe(), *NewDeactivatedBranchEnd.Describe() );
				SearchData.DeactivatedBranchEnd = NewDeactivatedBranchEnd;
			}
		}

		FBehaviorTreeInstance& ActiveInstance = InstanceStack[ActiveInstanceIdx];
		UBTCompositeNode* TestNode = ExecutionRequest.ExecuteNode;
		SearchData.AssignSearchId();
		SearchData.bPostponeSearch = false;
		SearchData.bSearchInProgress = true;
		// 设置当前执行节点为搜索根节点
		SearchData.SearchRootNode = FBTNodeIndex(ExecutionRequest.ExecuteInstanceIdx, ExecutionRequest.ExecuteNode->GetExecutionIndex());

		// activate root node if needed (can't be handled by parent composite...)
		// 如果激活的节点为空，设置为根节点
		if (ActiveInstance.ActiveNode == NULL)
		{
			ActiveInstance.ActiveNode = InstanceStack[ActiveInstanceIdx].RootNode;
			ActiveInstance.RootNode->OnNodeActivation(SearchData);
			BT_SEARCHLOG(SearchData, Verbose, TEXT("Activated root node: %s"), *UBehaviorTreeTypes::DescribeNodeHelper(ActiveInstance.RootNode));
		}

		// additional operations for restarting:
		// 重新启动的其他操作：
		if (!ExecutionRequest.bTryNextChild)
		{
			// mark all decorators less important than current search start node for removal
			const FBTNodeIndex DeactivateIdx(ExecutionRequest.SearchStart.InstanceIndex, ExecutionRequest.SearchStart.ExecutionIndex - 1);
			UnregisterAuxNodesUpTo(ExecutionRequest.SearchStart.ExecutionIndex ? DeactivateIdx : ExecutionRequest.SearchStart);

			// reactivate top search node, so it could use search range correctly
			BT_SEARCHLOG(SearchData, Verbose, TEXT("Reactivate node: %s [restart]"), *UBehaviorTreeTypes::DescribeNodeHelper(TestNode));
			ExecutionRequest.ExecuteNode->OnNodeRestart(SearchData);

			SearchData.SearchStart = ExecutionRequest.SearchStart;
			SearchData.SearchEnd = ExecutionRequest.SearchEnd;

			BT_SEARCHLOG(SearchData, Verbose, TEXT("Clamping search range: %s .. %s"),
				*SearchData.SearchStart.Describe(), *SearchData.SearchEnd.Describe());
		}
		else
		{
			// mark all decorators less important than current search start node for removal
			// (keep aux nodes for requesting node since it is higher priority)
			if (ExecutionRequest.ContinueWithResult == EBTNodeResult::Failed)
			{
				BT_SEARCHLOG(SearchData, Verbose, TEXT("Unregistering aux nodes up to %s"), *ExecutionRequest.SearchStart.Describe());
				UnregisterAuxNodesUpTo(ExecutionRequest.SearchStart);
			}

			// make sure it's reset before starting new search
			SearchData.SearchStart = FBTNodeIndex();
			SearchData.SearchEnd = FBTNodeIndex();
		}

		// store blackboard values from search start (can be changed by aux node removal/adding)
#if USE_BEHAVIORTREE_DEBUGGER
		StoreDebuggerBlackboard(SearchStartBlackboard);
#endif

		// start looking for next task
		while (TestNode && NextTask == NULL)
		{
			BT_SEARCHLOG(SearchData, Verbose, TEXT("Testing node: %s"), *UBehaviorTreeTypes::DescribeNodeHelper(TestNode));
			const int32 ChildBranchIdx = TestNode->FindChildToExecute(SearchData, NodeResult);
			UBTNode* StoreNode = TestNode;

			if (SearchData.bPostponeSearch)
			{
				// break out of current search loop
				TestNode = NULL;
				bIsSearchValid = false;
			}
			else if (ChildBranchIdx == BTSpecialChild::ReturnToParent)
			{
				UBTCompositeNode* ChildNode = TestNode;
				TestNode = TestNode->GetParentNode();

				// does it want to move up the tree?
				if (TestNode == NULL)
				{
					// special case for leaving instance: deactivate root manually
					ChildNode->OnNodeDeactivation(SearchData, NodeResult);

					// don't remove top instance from stack, so it could be looped
					if (ActiveInstanceIdx > 0)
					{
						StoreDebuggerSearchStep(InstanceStack[ActiveInstanceIdx].ActiveNode, ActiveInstanceIdx, NodeResult);
						StoreDebuggerRemovedInstance(ActiveInstanceIdx);
						InstanceStack[ActiveInstanceIdx].DeactivateNodes(SearchData, ActiveInstanceIdx);

						// store notify for later use if search is not reverted
						SearchData.PendingNotifies.Add(FBehaviorTreeSearchUpdateNotify(ActiveInstanceIdx, NodeResult));

						// and leave subtree
						ActiveInstanceIdx--;

						StoreDebuggerSearchStep(InstanceStack[ActiveInstanceIdx].ActiveNode, ActiveInstanceIdx, NodeResult);
						TestNode = InstanceStack[ActiveInstanceIdx].ActiveNode->GetParentNode();
					}
				}

				if (TestNode)
				{
					TestNode->OnChildDeactivation(SearchData, *ChildNode, NodeResult);
				}
			}
			else if (TestNode->Children.IsValidIndex(ChildBranchIdx))
			{
				// was new task found?
				NextTask = TestNode->Children[ChildBranchIdx].ChildTask;

				// or it wants to move down the tree?
				TestNode = TestNode->Children[ChildBranchIdx].ChildComposite;
			}

			// store after node deactivation had chance to modify result
			StoreDebuggerSearchStep(StoreNode, ActiveInstanceIdx, NodeResult);
		}

		// is search within requested bounds?
		if (NextTask)
		{
			const FBTNodeIndex NextTaskIdx(ActiveInstanceIdx, NextTask->GetExecutionIndex());
			bIsSearchValid = NextTaskIdx.TakesPriorityOver(ExecutionRequest.SearchEnd);
			
			// is new task is valid, but wants to ignore rerunning itself
			// check it's the same as active node (or any of active parallel tasks)
			if (bIsSearchValid && NextTask->ShouldIgnoreRestartSelf())
			{
				const bool bIsTaskRunning = InstanceStack[ActiveInstanceIdx].HasActiveNode(NextTaskIdx.ExecutionIndex);
				if (bIsTaskRunning)
				{
					BT_SEARCHLOG(SearchData, Verbose, TEXT("Task doesn't allow restart and it's already running! Discarding search."));
					bIsSearchValid = false;
				}
			}
		}

		// valid search - if search requires aborting current task and that abort happens to be latent
		// try to keep current (before search) state of tree until everything is ready for next execution
		// - observer changes will be applied just before starting new task (ProcessPendingExecution)
		// - memory needs to be updated as well, but this requires keeping another copy
		//   it's easier to just discard everything on first execution request and start new search when abort finishes

		if (!bIsSearchValid || SearchData.bPostponeSearch)
		{
			RollbackSearchChanges();

			UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("Search %s, reverted all changes."), !bIsSearchValid ? TEXT("is not valid") : TEXT("will be retried"));
		}

		SearchData.bSearchInProgress = false;
		// finish timer scope
	}

	if (!SearchData.bPostponeSearch)
	{
		// clear request accumulator
		ExecutionRequest = FBTNodeExecutionInfo();

		// unlock execution data, can get locked again if AbortCurrentTask starts any new requests
		PendingExecution.Unlock();

		if (bIsSearchValid)
		{
			// abort task if needed
			if (InstanceStack.Last().ActiveNodeType == EBTActiveNode::ActiveTask)
			{
				// prevent new execution requests for nodes inside the deactivated branch 
				// that may result from the aborted task.
				SearchData.bFilterOutRequestFromDeactivatedBranch = true;

				AbortCurrentTask();

				SearchData.bFilterOutRequestFromDeactivatedBranch = false;
			}

			// set next task to execute only when not lock for execution as everything has been cancelled/rollback
			if (!PendingExecution.IsLocked())
			{
				PendingExecution.NextTask = NextTask;
				PendingExecution.bOutOfNodes = (NextTask == NULL);
			}
		}

		ProcessPendingExecution();
	}
	else
	{
		// more important execution request was found
		// stop everything and search again in next tick
		ScheduleExecutionUpdate();
	}
}

// 处理上一个搜索任务中待执行的请求
void UBehaviorTreeComponent::ProcessPendingExecution()
{
	// can't continue if current task is still aborting
	if (bWaitingForAbortingTasks || !PendingExecution.IsSet())
	{
		return;
	}

	FBTPendingExecutionInfo SavedInfo = PendingExecution;
	PendingExecution = FBTPendingExecutionInfo();

	// collect all aux nodes that have lower priority than new task
	// occurs when normal execution is forced to revisit lower priority nodes (e.g. loop decorator)
	const FBTNodeIndex NextTaskIdx = SavedInfo.NextTask ? FBTNodeIndex(ActiveInstanceIdx, SavedInfo.NextTask->GetExecutionIndex()) : FBTNodeIndex(0, 0);
	UnregisterAuxNodesUpTo(NextTaskIdx);

	// change aux nodes
	ApplySearchData(SavedInfo.NextTask);

	// make sure that we don't have any additional instances on stack
	if (InstanceStack.Num() > (ActiveInstanceIdx + 1))
	{
		for (int32 InstanceIndex = ActiveInstanceIdx + 1; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
		{
			InstanceStack[InstanceIndex].Cleanup(*this, EBTMemoryClear::StoreSubtree);
		}

		InstanceStack.SetNum(ActiveInstanceIdx + 1);
	}

	// execute next task / notify out of nodes
	// validate active instance as well, execution can be delayed AND can have AbortCurrentTask call before using instance index
	if (SavedInfo.NextTask && InstanceStack.IsValidIndex(ActiveInstanceIdx))
	{
		ExecuteTask(SavedInfo.NextTask);
	}
	else
	{
		OnTreeFinished();
	}
}

// 恢复树的状态到搜索以前
void UBehaviorTreeComponent::RollbackSearchChanges()
{
	if (SearchData.RollbackInstanceIdx >= 0)
	{
		// 恢复回滚的数据
		ActiveInstanceIdx = SearchData.RollbackInstanceIdx;
		SearchData.DeactivatedBranchStart = SearchData.RollbackDeactivatedBranchStart;
		SearchData.DeactivatedBranchEnd = SearchData.RollbackDeactivatedBranchEnd;

		// 然后将回滚的数据清空
		SearchData.RollbackInstanceIdx = INDEX_NONE;
		SearchData.RollbackDeactivatedBranchStart = FBTNodeIndex();
		SearchData.RollbackDeactivatedBranchEnd = FBTNodeIndex();

		// 如果设置了激活节点不会回滚的标志
		if (SearchData.bPreserveActiveNodeMemoryOnRollback)
		{
			// 遍历所有的实例
			for (int32 Idx = 0; Idx < InstanceStack.Num(); Idx++)
			{
				// 注意这两个的不一致
				FBehaviorTreeInstance& InstanceData = InstanceStack[Idx];
				FBehaviorTreeInstanceId& InstanceInfo = KnownInstances[InstanceData.InstanceIdIndex];

				// 节点内存的尺寸
				const uint16 NodeMemorySize = InstanceData.ActiveNode ? InstanceData.ActiveNode->GetInstanceMemorySize() : 0;
				if (NodeMemorySize)
				{
					// copy over stored data in persistent, rollback is one time action and it won't be needed anymore
					// 以持久方式复制存储的数据，回滚是一项一次性的操作，因此不需要
					const uint8* NodeMemory = InstanceData.ActiveNode->GetNodeMemory<uint8>(InstanceData);
					uint8* DestMemory = InstanceInfo.InstanceMemory.GetData() + InstanceData.ActiveNode->GetMemoryOffset();

					FMemory::Memcpy(DestMemory, NodeMemory, NodeMemorySize);
				}
				// 重新设置实例内存
				InstanceData.SetInstanceMemory(InstanceInfo.InstanceMemory);
			}
		}
		else
		{
			// 持久实例内存拷贝到实例内存中
			CopyInstanceMemoryFromPersistent();
		}

		// apply new observer changes
		// 应用新的观察者变化
		ApplyDiscardedSearch();
	}
}

// 停用所有的节点一直到requested那个
bool UBehaviorTreeComponent::DeactivateUpTo(UBTCompositeNode* Node, uint16 NodeInstanceIdx, EBTNodeResult::Type& NodeResult, int32& OutLastDeactivatedChildIndex)
{
	UBTNode* DeactivatedChild = InstanceStack[ActiveInstanceIdx].ActiveNode;
	bool bDeactivateRoot = true;

	if (DeactivatedChild == NULL && ActiveInstanceIdx > NodeInstanceIdx)
	{
		// use tree's root node if instance didn't activated itself yet
		// 如果树实例没有激活，那就使用树的根节点
		DeactivatedChild = InstanceStack[ActiveInstanceIdx].RootNode;
		bDeactivateRoot = false;
	}

	while (DeactivatedChild)
	{
		// 得到父节点
		UBTCompositeNode* NotifyParent = DeactivatedChild->GetParentNode();
		if (NotifyParent)
		{
			OutLastDeactivatedChildIndex = NotifyParent->GetChildIndex(SearchData, *DeactivatedChild);
			// 停用子节点
			NotifyParent->OnChildDeactivation(SearchData, OutLastDeactivatedChildIndex, NodeResult);

			BT_SEARCHLOG(SearchData, Verbose, TEXT("Deactivate node: %s"), *UBehaviorTreeTypes::DescribeNodeHelper(DeactivatedChild));
			StoreDebuggerSearchStep(DeactivatedChild, ActiveInstanceIdx, NodeResult);
			DeactivatedChild = NotifyParent;
		}
		// 父节点为空了，表示已经到子树的根节点了
		else
		{
			// special case for leaving instance: deactivate root manually
			// 离开实例的特殊情况：手动停用根节点
			if (bDeactivateRoot)
			{
				InstanceStack[ActiveInstanceIdx].RootNode->OnNodeDeactivation(SearchData, NodeResult);
			}

			BT_SEARCHLOG(SearchData, Verbose, TEXT("%s node: %s [leave subtree]"),
				bDeactivateRoot ? TEXT("Deactivate") : TEXT("Skip over"),
				*UBehaviorTreeTypes::DescribeNodeHelper(InstanceStack[ActiveInstanceIdx].RootNode));

			// clear flag, it's valid only for newest instance
			// 清除标记，它只对最近的实例有效
			bDeactivateRoot = true;

			// shouldn't happen, but it's better to have built in failsafe just in case
			// 应该不会发生，但是最好内置故障安全功能，以防万一 
			if (ActiveInstanceIdx == 0)
			{
				BT_SEARCHLOG(SearchData, Error, TEXT("Execution path does NOT contain common parent node, restarting tree! AI:%s"),
					*GetNameSafe(SearchData.OwnerComp.GetOwner()));

				RestartTree();
				return false;
			}

			// store notify for later use if search is not reverted
			// 如果未回滚搜索，则存储通知以供以后使用
			SearchData.PendingNotifies.Add(FBehaviorTreeSearchUpdateNotify(ActiveInstanceIdx, NodeResult));

			ActiveInstanceIdx--;
			DeactivatedChild = InstanceStack[ActiveInstanceIdx].ActiveNode;
		}
		// 找到对应的节点，跳出循环
		if (DeactivatedChild == Node)
		{
			break;
		}
	}

	return true;
}

void UBehaviorTreeComponent::UnregisterAuxNodesUpTo(const FBTNodeIndex& Index)
{
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		for (const UBTAuxiliaryNode* AuxNode : InstanceInfo.GetActiveAuxNodes())
		{
			FBTNodeIndex AuxIdx(InstanceIndex, AuxNode->GetExecutionIndex());
			if (Index.TakesPriorityOver(AuxIdx))
			{
				SearchData.AddUniqueUpdate(FBehaviorTreeSearchUpdate(AuxNode, InstanceIndex, EBTNodeUpdateMode::Remove));
			}
		}
	}
}

void UBehaviorTreeComponent::UnregisterAuxNodesInRange(const FBTNodeIndex& FromIndex, const FBTNodeIndex& ToIndex)
{
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		for (const UBTAuxiliaryNode* AuxNode : InstanceInfo.GetActiveAuxNodes())
		{
			FBTNodeIndex AuxIdx(InstanceIndex, AuxNode->GetExecutionIndex());
			if (FromIndex.TakesPriorityOver(AuxIdx) && AuxIdx.TakesPriorityOver(ToIndex))
			{
				SearchData.AddUniqueUpdate(FBehaviorTreeSearchUpdate(AuxNode, InstanceIndex, EBTNodeUpdateMode::Remove));
			}
		}
	}
}

void UBehaviorTreeComponent::UnregisterAuxNodesInBranch(const UBTCompositeNode* Node, bool bApplyImmediately)
{
	const int32 InstanceIdx = FindInstanceContainingNode(Node);
	if (InstanceIdx != INDEX_NONE)
	{
		check(Node);

		TArray<FBehaviorTreeSearchUpdate> UpdateListCopy;
		if (bApplyImmediately)
		{
			UpdateListCopy = SearchData.PendingUpdates;
			SearchData.PendingUpdates.Reset();
		}

		const FBTNodeIndex FromIndex(InstanceIdx, Node->GetExecutionIndex());
		const FBTNodeIndex ToIndex(InstanceIdx, Node->GetLastExecutionIndex());
		UnregisterAuxNodesInRange(FromIndex, ToIndex);

		if (bApplyImmediately)
		{
			ApplySearchUpdates(SearchData.PendingUpdates, 0);
			SearchData.PendingUpdates = UpdateListCopy;
		}
	}
}

bool UBehaviorTreeComponent::ProcessPendingUnregister()
{
	if (PendingUnregisterAuxNodesRequests.Ranges.Num() == 0)
	{
		// no work done
		return false;
	}

	TGuardValue<TArray<FBehaviorTreeSearchUpdate>> ScopedList(SearchData.PendingUpdates, {});

	for (const FBTNodeIndexRange& Range : PendingUnregisterAuxNodesRequests.Ranges)
	{
		UnregisterAuxNodesInRange(Range.FromIndex, Range.ToIndex);
	}
	PendingUnregisterAuxNodesRequests = {};

	ApplySearchUpdates(SearchData.PendingUpdates, 0);

	// has done work
	return true;
}

void UBehaviorTreeComponent::ExecuteTask(UBTTaskNode* TaskNode)
{
	SCOPE_CYCLE_COUNTER(STAT_AI_BehaviorTree_ExecutionTime);

	// We expect that there should be valid instances on the stack
	if (!ensure(InstanceStack.IsValidIndex(ActiveInstanceIdx)))
	{
		return;
	}

	FBehaviorTreeInstance& ActiveInstance = InstanceStack[ActiveInstanceIdx];

	// task service activation is not part of search update (although deactivation is, through DeactivateUpTo), start them before execution
	for (int32 ServiceIndex = 0; ServiceIndex < TaskNode->Services.Num(); ServiceIndex++)
	{
		UBTService* ServiceNode = TaskNode->Services[ServiceIndex];
		uint8* NodeMemory = (uint8*)ServiceNode->GetNodeMemory<uint8>(ActiveInstance);

		ActiveInstance.AddToActiveAuxNodes(ServiceNode);

		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Activating task service: %s"), *UBehaviorTreeTypes::DescribeNodeHelper(ServiceNode));
		ServiceNode->WrappedOnBecomeRelevant(*this, NodeMemory);
	}

	ActiveInstance.ActiveNode = TaskNode;
	ActiveInstance.ActiveNodeType = EBTActiveNode::ActiveTask;

	// make a snapshot for debugger
	StoreDebuggerExecutionStep(EBTExecutionSnap::Regular);

	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Execute task: %s"), *UBehaviorTreeTypes::DescribeNodeHelper(TaskNode));

	// store instance before execution, it could result in pushing a subtree
	uint16 InstanceIdx = ActiveInstanceIdx;

	EBTNodeResult::Type TaskResult;
	{
		SCOPE_CYCLE_UOBJECT(TaskNode, TaskNode);
		uint8* NodeMemory = (uint8*)(TaskNode->GetNodeMemory<uint8>(ActiveInstance));
		TaskResult = TaskNode->WrappedExecuteTask(*this, NodeMemory);
	}

	// pass task finished if wasn't already notified (FinishLatentTask)
	const UBTNode* ActiveNodeAfterExecution = GetActiveNode();
	if (ActiveNodeAfterExecution == TaskNode)
	{
		// update task's runtime values after it had a chance to initialize memory
		UpdateDebuggerAfterExecution(TaskNode, InstanceIdx);

		OnTaskFinished(TaskNode, TaskResult);
	}
}

void UBehaviorTreeComponent::AbortCurrentTask()
{
	const int32 CurrentInstanceIdx = InstanceStack.Num() - 1;
	FBehaviorTreeInstance& CurrentInstance = InstanceStack[CurrentInstanceIdx];
	CurrentInstance.ActiveNodeType = EBTActiveNode::AbortingTask;

	UBTTaskNode* CurrentTask = (UBTTaskNode*)CurrentInstance.ActiveNode;

	// remove all observers before requesting abort
	UnregisterMessageObserversFrom(CurrentTask);

	// protect memory of this task from rollbacks
	// at this point, invalid search rollback already happened
	// only reason to do the rollback is restoring tree state during abort for accumulated requests
	// but this task needs to remain unchanged: it's still aborting and internal memory can be modified on AbortTask call
	SearchData.bPreserveActiveNodeMemoryOnRollback = true;

	UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Abort task: %s"), *UBehaviorTreeTypes::DescribeNodeHelper(CurrentTask));

	// abort task using current state of tree
	uint8* NodeMemory = (uint8*)(CurrentTask->GetNodeMemory<uint8>(CurrentInstance));
	EBTNodeResult::Type TaskResult = CurrentTask->WrappedAbortTask(*this, NodeMemory);

	// pass task finished if wasn't already notified (FinishLatentAbort)
	if (CurrentInstance.ActiveNodeType == EBTActiveNode::AbortingTask &&
		CurrentInstanceIdx == (InstanceStack.Num() - 1))
	{
		OnTaskFinished(CurrentTask, TaskResult);
	}
}

void UBehaviorTreeComponent::RegisterMessageObserver(const UBTTaskNode* TaskNode, FName MessageType)
{
	if (TaskNode)
	{
		FBTNodeIndex NodeIdx;
		NodeIdx.ExecutionIndex = TaskNode->GetExecutionIndex();
		NodeIdx.InstanceIndex = InstanceStack.Num() - 1;

		TaskMessageObservers.Add(NodeIdx,
			FAIMessageObserver::Create(this, MessageType, FOnAIMessage::CreateUObject(const_cast<UBTTaskNode*>(TaskNode), &UBTTaskNode::ReceivedMessage))
			);

		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Message[%s] observer added for %s"),
			*MessageType.ToString(), *UBehaviorTreeTypes::DescribeNodeHelper(TaskNode));
	}
}

void UBehaviorTreeComponent::RegisterMessageObserver(const UBTTaskNode* TaskNode, FName MessageType, FAIRequestID RequestID)
{
	if (TaskNode)
	{
		FBTNodeIndex NodeIdx;
		NodeIdx.ExecutionIndex = TaskNode->GetExecutionIndex();
		NodeIdx.InstanceIndex = InstanceStack.Num() - 1;

		TaskMessageObservers.Add(NodeIdx,
			FAIMessageObserver::Create(this, MessageType, RequestID, FOnAIMessage::CreateUObject(const_cast<UBTTaskNode*>(TaskNode), &UBTTaskNode::ReceivedMessage))
			);

		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Message[%s:%d] observer added for %s"),
			*MessageType.ToString(), RequestID, *UBehaviorTreeTypes::DescribeNodeHelper(TaskNode));
	}
}

// 删除任务注册的消息观察者
void UBehaviorTreeComponent::UnregisterMessageObserversFrom(const FBTNodeIndex& TaskIdx)
{
	// 从消息观察者map中删除
	const int32 NumRemoved = TaskMessageObservers.Remove(TaskIdx);
	if (NumRemoved)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Message observers removed for task[%d:%d] (num:%d)"),
			TaskIdx.InstanceIndex, TaskIdx.ExecutionIndex, NumRemoved);
	}
}

// 删除任务注册的消息观察者
void UBehaviorTreeComponent::UnregisterMessageObserversFrom(const UBTTaskNode* TaskNode)
{
	if (TaskNode && InstanceStack.Num())
	{
		const FBehaviorTreeInstance& ActiveInstance = InstanceStack.Last();

		// 建立节点索引，执行索引喝实例索引
		FBTNodeIndex NodeIdx;
		NodeIdx.ExecutionIndex = TaskNode->GetExecutionIndex();
		NodeIdx.InstanceIndex = FindInstanceContainingNode(TaskNode);
		
		UnregisterMessageObserversFrom(NodeIdx);
	}
}

void UBehaviorTreeComponent::RegisterParallelTask(const UBTTaskNode* TaskNode)
{
	if (InstanceStack.IsValidIndex(ActiveInstanceIdx))
	{
		FBehaviorTreeInstance& InstanceInfo = InstanceStack[ActiveInstanceIdx];
		InstanceInfo.AddToParallelTasks(FBehaviorTreeParallelTask(TaskNode, EBTTaskStatus::Active));

		UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("Parallel task: %s added to active list"),
			*UBehaviorTreeTypes::DescribeNodeHelper(TaskNode));

		if (InstanceInfo.ActiveNode == TaskNode)
		{
			// switch to inactive state, so it could start background tree
			InstanceInfo.ActiveNodeType = EBTActiveNode::InactiveTask;
		}
	}
}

void UBehaviorTreeComponent::UnregisterParallelTask(const UBTTaskNode* TaskNode, uint16 InstanceIdx)
{
	bool bShouldUpdate = false;
	if (InstanceStack.IsValidIndex(InstanceIdx))
	{
		FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIdx];
		for (int32 TaskIndex = InstanceInfo.GetParallelTasks().Num() - 1; TaskIndex >= 0; TaskIndex--)
		{
			if (InstanceInfo.GetParallelTasks()[TaskIndex].TaskNode == TaskNode)
			{
				UE_VLOG(GetOwner(), LogBehaviorTree, Verbose, TEXT("Parallel task: %s removed from active list"),
					*UBehaviorTreeTypes::DescribeNodeHelper(TaskNode));

				InstanceInfo.RemoveParallelTaskAt(TaskIndex);
				bShouldUpdate = true;
				break;
			}
		}
	}

	if (bShouldUpdate)
	{
		UpdateAbortingTasks();
	}
}

// 更新中止任务的状态
void UBehaviorTreeComponent::UpdateAbortingTasks()
{
	// 当前任务是否为中止任务
	bWaitingForAbortingTasks = InstanceStack.Num() ? (InstanceStack.Last().ActiveNodeType == EBTActiveNode::AbortingTask) : false;
	// 遍历所有的实例
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num() && !bWaitingForAbortingTasks; InstanceIndex++)
	{
		FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		// 遍历所有实例中的并行任务
		for (const FBehaviorTreeParallelTask& ParallelInfo : InstanceInfo.GetParallelTasks())
		{
			if (ParallelInfo.Status == EBTTaskStatus::Aborting)
			{
				bWaitingForAbortingTasks = true;
				break;
			}
		}
	}
}

// 在执行堆栈上推送行为树实例
bool UBehaviorTreeComponent::PushInstance(UBehaviorTree& TreeAsset)
{
	// check if blackboard class match
	// 检查黑板组件是否匹配
	if (TreeAsset.BlackboardAsset && BlackboardComp && !BlackboardComp->IsCompatibleWith(TreeAsset.BlackboardAsset))
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Warning, TEXT("Failed to execute tree %s: blackboard %s is not compatibile with current: %s!"),
			*TreeAsset.GetName(), *GetNameSafe(TreeAsset.BlackboardAsset), *GetNameSafe(BlackboardComp->GetBlackboardAsset()));

		return false;
	}

	// 行为树管理器
	UBehaviorTreeManager* BTManager = UBehaviorTreeManager::GetCurrent(GetWorld());
	if (BTManager == NULL)
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Warning, TEXT("Failed to execute tree %s: behavior tree manager not found!"), *TreeAsset.GetName());
		return false;
	}

	// check if parent node allows it
	// 检查父节点是否允许它
	const UBTNode* ActiveNode = GetActiveNode();
	// 得到父节点
	const UBTCompositeNode* ActiveParent = ActiveNode ? ActiveNode->GetParentNode() : NULL;
	if (ActiveParent)
	{
		// 返回节点内存
		uint8* ParentMemory = GetNodeMemory((UBTNode*)ActiveParent, InstanceStack.Num() - 1);
		// 得到子节点的索引
		int32 ChildIdx = ActiveNode ? ActiveParent->GetChildIndex(*ActiveNode) : INDEX_NONE;
		// 是否可以执行新的子树
		const bool bIsAllowed = ActiveParent->CanPushSubtree(*this, ParentMemory, ChildIdx);
		if (!bIsAllowed)
		{
			UE_VLOG(GetOwner(), LogBehaviorTree, Warning, TEXT("Failed to execute tree %s: parent of active node does not allow it! (%s)"),
				*TreeAsset.GetName(), *UBehaviorTreeTypes::DescribeNodeHelper(ActiveParent));
			return false;
		}
	}

	UBTCompositeNode* RootNode = NULL;
	uint16 InstanceMemorySize = 0;
	// 加载树
	const bool bLoaded = BTManager->LoadTree(TreeAsset, RootNode, InstanceMemorySize);
	if (bLoaded)
	{
		// 新实例
		FBehaviorTreeInstance NewInstance;
		NewInstance.InstanceIdIndex = UpdateInstanceId(&TreeAsset, ActiveNode, InstanceStack.Num() - 1);
		NewInstance.RootNode = RootNode;
		NewInstance.ActiveNode = NULL;
		NewInstance.ActiveNodeType = EBTActiveNode::Composite;

		// initialize memory and node instances
		// 初始化内存和节点实例
		FBehaviorTreeInstanceId& InstanceInfo = KnownInstances[NewInstance.InstanceIdIndex];
		int32 NodeInstanceIndex = InstanceInfo.FirstNodeInstance;
		const bool bFirstTime = (InstanceInfo.InstanceMemory.Num() != InstanceMemorySize);
		if (bFirstTime)
		{
			InstanceInfo.InstanceMemory.AddZeroed(InstanceMemorySize);
			InstanceInfo.RootNode = RootNode;
		}
		// 设置实例内存
		NewInstance.SetInstanceMemory(InstanceInfo.InstanceMemory);
		// 初始化内存并创建节点实例
		NewInstance.Initialize(*this, *RootNode, NodeInstanceIndex, bFirstTime ? EBTMemoryInit::Initialize : EBTMemoryInit::RestoreSubtree);
		// 压入实例数组
		InstanceStack.Push(NewInstance);
		ActiveInstanceIdx = InstanceStack.Num() - 1;

		// start root level services now (they won't be removed on looping tree anyway)
		// 启动根级别的服务节点
		for (int32 ServiceIndex = 0; ServiceIndex < RootNode->Services.Num(); ServiceIndex++)
		{
			UBTService* ServiceNode = RootNode->Services[ServiceIndex];
			uint8* NodeMemory = (uint8*)ServiceNode->GetNodeMemory<uint8>(InstanceStack[ActiveInstanceIdx]);

			// send initial on search start events in case someone is using them for init logic
			// 发送初始搜索开始事件，以防有人将其用于初始化逻辑 
			ServiceNode->NotifyParentActivation(SearchData);

			InstanceStack[ActiveInstanceIdx].AddToActiveAuxNodes(ServiceNode);
			ServiceNode->WrappedOnBecomeRelevant(*this, NodeMemory);
		}

		FBehaviorTreeDelegates::OnTreeStarted.Broadcast(*this, TreeAsset);

		// start new task
		// 启动新任务
		RequestExecution(RootNode, ActiveInstanceIdx, RootNode, 0, EBTNodeResult::InProgress);
		return true;
	}

	return false;
}

// 将新创建的子树的唯一ID添加到KnownInstances列表中并返回其索引 
uint8 UBehaviorTreeComponent::UpdateInstanceId(UBehaviorTree* TreeAsset, const UBTNode* OriginNode, int32 OriginInstanceIdx)
{
	// 行为树实例的标志符
	FBehaviorTreeInstanceId InstanceId;
	InstanceId.TreeAsset = TreeAsset;

	// build path from origin node
	// 从原始节点创建路径
	{
		const uint16 ExecutionIndex = OriginNode ? OriginNode->GetExecutionIndex() : MAX_uint16;
		InstanceId.Path.Add(ExecutionIndex);
	}
	// 遍历所有实例的激活节点
	for (int32 InstanceIndex = OriginInstanceIdx - 1; InstanceIndex >= 0; InstanceIndex--)
	{
		const uint16 ExecutionIndex = InstanceStack[InstanceIndex].ActiveNode ? InstanceStack[InstanceIndex].ActiveNode->GetExecutionIndex() : MAX_uint16;
		InstanceId.Path.Add(ExecutionIndex);
	}

	// try to find matching existing Id
	// 尝试从存在的ID中找匹配的
	for (int32 InstanceIndex = 0; InstanceIndex < KnownInstances.Num(); InstanceIndex++)
	{
		if (KnownInstances[InstanceIndex] == InstanceId)
		{
			return InstanceIndex;
		}
	}

	// add new one
	// 增加子树实例
	InstanceId.FirstNodeInstance = NodeInstances.Num();
	const int32 NewIndex = KnownInstances.Add(InstanceId);
	check(NewIndex < MAX_uint8);
	return NewIndex;
}

// 在上下文环境中找到行为树实例
int32 UBehaviorTreeComponent::FindInstanceContainingNode(const UBTNode* Node) const
{
	int32 InstanceIdx = INDEX_NONE;
	// 查找模板节点
	const UBTNode* TemplateNode = FindTemplateNode(Node);
	if (TemplateNode && InstanceStack.Num())
	{
		// 如果活动节点不是对应节点的模板节点
		if (InstanceStack[ActiveInstanceIdx].ActiveNode != TemplateNode)
		{
			// 找到根节点
			const UBTNode* RootNode = TemplateNode;
			while (RootNode->GetParentNode())
			{
				RootNode = RootNode->GetParentNode();
			}

			// 遍历所有的树实例，查看当前的节点属于哪个树实例
			for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
			{
				if (InstanceStack[InstanceIndex].RootNode == RootNode)
				{
					InstanceIdx = InstanceIndex;
					break;
				}
			}
		}
		else
		{
			InstanceIdx = ActiveInstanceIdx;
		}
	}

	return InstanceIdx;
}

// 对于给定的实例节点查找对应的模板节点
UBTNode* UBehaviorTreeComponent::FindTemplateNode(const UBTNode* Node) const
{
	if (Node == NULL || !Node->IsInstanced() || Node->GetParentNode() == NULL)
	{
		return (UBTNode*)Node;
	}

	// 先找到父节点
	UBTCompositeNode* ParentNode = Node->GetParentNode();
	// 遍历父节点的所有直接点
	for (int32 ChildIndex = 0; ChildIndex < ParentNode->Children.Num(); ChildIndex++)
	{
		FBTCompositeChild& ChildInfo = ParentNode->Children[ChildIndex];

		// Task节点
		if (ChildInfo.ChildTask)
		{
			// 如果节点就是Task节点，并且有相同的执行索引
			if (ChildInfo.ChildTask->GetExecutionIndex() == Node->GetExecutionIndex())
			{
				return ChildInfo.ChildTask;
			}

			// 如果是服务节点，，找到相同的执行索引
			for (int32 ServiceIndex = 0; ServiceIndex < ChildInfo.ChildTask->Services.Num(); ServiceIndex++)
			{
				if (ChildInfo.ChildTask->Services[ServiceIndex]->GetExecutionIndex() == Node->GetExecutionIndex())
				{
					return ChildInfo.ChildTask->Services[ServiceIndex];
				}
			}
		}

		// 或者是装饰节点
		for (int32 DecoratorIndex = 0; DecoratorIndex < ChildInfo.Decorators.Num(); DecoratorIndex++)
		{
			if (ChildInfo.Decorators[DecoratorIndex]->GetExecutionIndex() == Node->GetExecutionIndex())
			{
				return ChildInfo.Decorators[DecoratorIndex];
			}
		}
	}
	
	// 或者是父节点的服务节点
	for (int32 ServiceIndex = 0; ServiceIndex < ParentNode->Services.Num(); ServiceIndex++)
	{
		if (ParentNode->Services[ServiceIndex]->GetExecutionIndex() == Node->GetExecutionIndex())
		{
			return ParentNode->Services[ServiceIndex];
		}
	}

	return NULL;
}

// 返回节点内存
uint8* UBehaviorTreeComponent::GetNodeMemory(UBTNode* Node, int32 InstanceIdx) const
{
	return InstanceStack.IsValidIndex(InstanceIdx) ? (uint8*)Node->GetNodeMemory<uint8>(InstanceStack[InstanceIdx]) : NULL;
}

// 删除实例节点，已知的子树实例和安全地清空持久化内存
void UBehaviorTreeComponent::RemoveAllInstances()
{
	// 如果还有实例化得行为树，表示还在运行，就得先停掉
	if (InstanceStack.Num())
	{
		StopTree(EBTStopMode::Forced);
	}

	FBehaviorTreeInstance DummyInstance;
	for (int32 Idx = 0; Idx < KnownInstances.Num(); Idx++)
	{
		const FBehaviorTreeInstanceId& Info = KnownInstances[Idx];
		if (Info.InstanceMemory.Num())
		{
			// instance memory will be removed on Cleanup in EBTMemoryClear::Destroy mode
			// prevent from calling it multiple times - StopTree does it for current InstanceStack
			// 实例内存将在EBTMemoryClear :: Destroy模式下的Cleanup上被删除，
			// 以防止多次调用它-StopTree为当前InstanceStack进行调用 
			DummyInstance.SetInstanceMemory(Info.InstanceMemory);
			DummyInstance.InstanceIdIndex = Idx;
			DummyInstance.RootNode = Info.RootNode;

			DummyInstance.Cleanup(*this, EBTMemoryClear::Destroy);
		}
	}
	// 将实例和节点都重置了
	KnownInstances.Reset();
	NodeInstances.Reset();
}

// 从运行实例中将内存拷贝到持久化内存中
void UBehaviorTreeComponent::CopyInstanceMemoryToPersistent()
{
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstance& InstanceData = InstanceStack[InstanceIndex];
		FBehaviorTreeInstanceId& InstanceInfo = KnownInstances[InstanceData.InstanceIdIndex];

		InstanceInfo.InstanceMemory = InstanceData.GetInstanceMemory();
	}
}

// 将持久化内存内存块拷贝到运行实例中
void UBehaviorTreeComponent::CopyInstanceMemoryFromPersistent()
{
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		FBehaviorTreeInstance& InstanceData = InstanceStack[InstanceIndex];
		const FBehaviorTreeInstanceId& InstanceInfo = KnownInstances[InstanceData.InstanceIdIndex];

		InstanceData.SetInstanceMemory(InstanceInfo.InstanceMemory);
	}
}

FString UBehaviorTreeComponent::GetDebugInfoString() const 
{ 
	FString DebugInfo;
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstance& InstanceData = InstanceStack[InstanceIndex];
		const FBehaviorTreeInstanceId& InstanceInfo = KnownInstances[InstanceData.InstanceIdIndex];
		DebugInfo += FString::Printf(TEXT("Behavior tree: %s\n"), *GetNameSafe(InstanceInfo.TreeAsset));

		UBTNode* Node = InstanceData.ActiveNode;
		FString NodeTrace;

		while (Node)
		{
			uint8* NodeMemory = (uint8*)(Node->GetNodeMemory<uint8>(InstanceData));
			NodeTrace = FString::Printf(TEXT("  %s\n"), *Node->GetRuntimeDescription(*this, NodeMemory, EBTDescriptionVerbosity::Basic)) + NodeTrace;
			Node = Node->GetParentNode();
		}

		DebugInfo += NodeTrace;
	}

	return DebugInfo;
}

FString UBehaviorTreeComponent::DescribeActiveTasks() const
{
	FString ActiveTask(TEXT("None"));
	if (InstanceStack.Num())
	{
		const FBehaviorTreeInstance& LastInstance = InstanceStack.Last();
		if (LastInstance.ActiveNodeType == EBTActiveNode::ActiveTask)
		{
			ActiveTask = UBehaviorTreeTypes::DescribeNodeHelper(LastInstance.ActiveNode);
		}
	}

	FString ParallelTasks;
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		for (const FBehaviorTreeParallelTask& ParallelInfo : InstanceInfo.GetParallelTasks())
		{
			if (ParallelInfo.Status == EBTTaskStatus::Active)
			{
				ParallelTasks += UBehaviorTreeTypes::DescribeNodeHelper(ParallelInfo.TaskNode);
				ParallelTasks += TEXT(", ");
			}
		}
	}

	if (ParallelTasks.Len() > 0)
	{
		ActiveTask += TEXT(" (");
		ActiveTask += ParallelTasks.LeftChop(2);
		ActiveTask += TEXT(')');
	}

	return ActiveTask;
}

FString UBehaviorTreeComponent::DescribeActiveTrees() const
{
	FString Assets;
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstanceId& InstanceInfo = KnownInstances[InstanceStack[InstanceIndex].InstanceIdIndex];
		Assets += InstanceInfo.TreeAsset->GetName();
		Assets += TEXT(", ");
	}

	return Assets.Len() ? Assets.LeftChop(2) : TEXT("None");
}

float UBehaviorTreeComponent::GetTagCooldownEndTime(FGameplayTag CooldownTag) const
{
	const float CooldownEndTime = CooldownTagsMap.FindRef(CooldownTag);
	return CooldownEndTime;
}

void UBehaviorTreeComponent::AddCooldownTagDuration(FGameplayTag CooldownTag, float CooldownDuration, bool bAddToExistingDuration)
{
	if (CooldownTag.IsValid())
	{
		float* CurrentEndTime = CooldownTagsMap.Find(CooldownTag);

		// If we are supposed to add to an existing duration, do that, otherwise we set a new value.
		if (bAddToExistingDuration && (CurrentEndTime != nullptr))
		{
			*CurrentEndTime += CooldownDuration;
		}
		else
		{
			CooldownTagsMap.Add(CooldownTag, (GetWorld()->GetTimeSeconds() + CooldownDuration));
		}
	}
}

bool SetDynamicSubtreeHelper(const UBTCompositeNode* TestComposite,
	const FBehaviorTreeInstance& InstanceInfo, const UBehaviorTreeComponent* OwnerComp,
	const FGameplayTag& InjectTag, UBehaviorTree* BehaviorAsset)
{
	bool bInjected = false;

	for (int32 Idx = 0; Idx < TestComposite->Children.Num(); Idx++)
	{
		const FBTCompositeChild& ChildInfo = TestComposite->Children[Idx];
		if (ChildInfo.ChildComposite)
		{
			bInjected = (SetDynamicSubtreeHelper(ChildInfo.ChildComposite, InstanceInfo, OwnerComp, InjectTag, BehaviorAsset) || bInjected);
		}
		else
		{
			UBTTask_RunBehaviorDynamic* SubtreeTask = Cast<UBTTask_RunBehaviorDynamic>(ChildInfo.ChildTask);
			if (SubtreeTask && SubtreeTask->HasMatchingTag(InjectTag))
			{
				const uint8* NodeMemory = SubtreeTask->GetNodeMemory<uint8>(InstanceInfo);
				UBTTask_RunBehaviorDynamic* InstancedNode = Cast<UBTTask_RunBehaviorDynamic>(SubtreeTask->GetNodeInstance(*OwnerComp, (uint8*)NodeMemory));
				if (InstancedNode)
				{
					const bool bAssetChanged = InstancedNode->SetBehaviorAsset(BehaviorAsset);
					if (bAssetChanged)
					{
						UE_VLOG(OwnerComp->GetOwner(), LogBehaviorTree, Log, TEXT("Replaced subtree in %s with %s (tag: %s)"),
							*UBehaviorTreeTypes::DescribeNodeHelper(SubtreeTask), *GetNameSafe(BehaviorAsset), *InjectTag.ToString());
						bInjected = true;
					}
				}
			}
		}
	}

	return bInjected;
}

void UBehaviorTreeComponent::SetDynamicSubtree(FGameplayTag InjectTag, UBehaviorTree* BehaviorAsset)
{
	bool bInjected = false;
	// replace at matching injection points
	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		bInjected = (SetDynamicSubtreeHelper(InstanceInfo.RootNode, InstanceInfo, this, InjectTag, BehaviorAsset) || bInjected);
	}

	// restart subtree if it was replaced
	if (bInjected)
	{
		for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
		{
			const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
			if (InstanceInfo.ActiveNodeType == EBTActiveNode::ActiveTask)
			{
				const UBTTask_RunBehaviorDynamic* SubtreeTask = Cast<const UBTTask_RunBehaviorDynamic>(InstanceInfo.ActiveNode);
				if (SubtreeTask && SubtreeTask->HasMatchingTag(InjectTag))
				{
					UBTCompositeNode* RestartNode = SubtreeTask->GetParentNode();
					int32 RestartChildIdx = RestartNode->GetChildIndex(*SubtreeTask);

					RequestExecution(RestartNode, InstanceIndex, SubtreeTask, RestartChildIdx, EBTNodeResult::Aborted);
					break;
				}
			}
		}
	}
	else
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Log, TEXT("Failed to inject subtree %s at tag %s"), *GetNameSafe(BehaviorAsset), *InjectTag.ToString());
	}
}

#if ENABLE_VISUAL_LOG
void UBehaviorTreeComponent::DescribeSelfToVisLog(FVisualLogEntry* Snapshot) const
{
	if (IsPendingKill())
	{
		return;
	}
	
	Super::DescribeSelfToVisLog(Snapshot);

	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIndex];
		const FBehaviorTreeInstanceId& InstanceId = KnownInstances[InstanceInfo.InstanceIdIndex];

		FVisualLogStatusCategory StatusCategory;
		StatusCategory.Category = FString::Printf(TEXT("BehaviorTree %d (asset: %s)"), InstanceIndex, *GetNameSafe(InstanceId.TreeAsset));

		if (InstanceInfo.GetActiveAuxNodes().Num() > 0)
		{
			FString ObserversDesc;
			for (const UBTAuxiliaryNode* AuxNode : InstanceInfo.GetActiveAuxNodes())
			{
				ObserversDesc += FString::Printf(TEXT("%d. %s\n"), AuxNode->GetExecutionIndex(), *AuxNode->GetNodeName(), *AuxNode->GetStaticDescription());
			}
			StatusCategory.Add(TEXT("Observers"), ObserversDesc);
		}

		TArray<FString> Descriptions;
		UBTNode* Node = InstanceInfo.ActiveNode;
		while (Node)
		{
			uint8* NodeMemory = (uint8*)(Node->GetNodeMemory<uint8>(InstanceInfo));
			Descriptions.Add(Node->GetRuntimeDescription(*this, NodeMemory, EBTDescriptionVerbosity::Detailed));
		
			Node = Node->GetParentNode();
		}

		for (int32 DescriptionIndex = Descriptions.Num() - 1; DescriptionIndex >= 0; DescriptionIndex--)
		{
			int32 SplitIdx = INDEX_NONE;
			if (Descriptions[DescriptionIndex].FindChar(TEXT(','), SplitIdx))
			{
				const FString KeyDesc = Descriptions[DescriptionIndex].Left(SplitIdx);
				const FString ValueDesc = Descriptions[DescriptionIndex].Mid(SplitIdx + 1).TrimStart();

				StatusCategory.Add(KeyDesc, ValueDesc);
			}
			else
			{
				StatusCategory.Add(Descriptions[DescriptionIndex], TEXT(""));
			}
		}

		if (StatusCategory.Data.Num() == 0)
		{
			StatusCategory.Add(TEXT("root"), TEXT("not initialized"));
		}

		Snapshot->Status.Add(StatusCategory);
	}

	if (CooldownTagsMap.Num() > 0)
	{
		FVisualLogStatusCategory StatusCategory;
		StatusCategory.Category = TEXT("Cooldown Tags");

		for (const auto& CooldownTagPair : CooldownTagsMap)
		{
			const FString TimeStr = FString::Printf(TEXT("%.2fs"), CooldownTagPair.Value);
			StatusCategory.Add(CooldownTagPair.Key.ToString(), TimeStr);
		}

		Snapshot->Status.Add(StatusCategory);
	}
}

#endif // ENABLE_VISUAL_LOG

void UBehaviorTreeComponent::StoreDebuggerExecutionStep(EBTExecutionSnap::Type SnapType)
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!IsDebuggerActive())
	{
		return;
	}

	FBehaviorTreeExecutionStep CurrentStep;
	CurrentStep.ExecutionStepId = DebuggerSteps.Num() ? DebuggerSteps.Last().ExecutionStepId + 1 : 0;
	CurrentStep.TimeStamp = GetWorld()->GetTimeSeconds();
	CurrentStep.BlackboardValues = SearchStartBlackboard;

	for (int32 InstanceIndex = 0; InstanceIndex < InstanceStack.Num(); InstanceIndex++)
	{
		const FBehaviorTreeInstance& ActiveInstance = InstanceStack[InstanceIndex];
		
		FBehaviorTreeDebuggerInstance StoreInfo;
		StoreDebuggerInstance(StoreInfo, InstanceIndex, SnapType);
		CurrentStep.InstanceStack.Add(StoreInfo);
	}

	for (int32 InstanceIndex = RemovedInstances.Num() - 1; InstanceIndex >= 0; InstanceIndex--)
	{
		CurrentStep.InstanceStack.Add(RemovedInstances[InstanceIndex]);
	}

	CurrentSearchFlow.Reset();
	CurrentRestarts.Reset();
	RemovedInstances.Reset();

	UBehaviorTreeManager* ManagerCDO = (UBehaviorTreeManager*)UBehaviorTreeManager::StaticClass()->GetDefaultObject();
	while (DebuggerSteps.Num() >= ManagerCDO->MaxDebuggerSteps)
	{
		DebuggerSteps.RemoveAt(0, /*Count=*/1, /*bAllowShrinking=*/false);
	}
	DebuggerSteps.Add(CurrentStep);
#endif
}

void UBehaviorTreeComponent::StoreDebuggerInstance(FBehaviorTreeDebuggerInstance& InstanceInfo, uint16 InstanceIdx, EBTExecutionSnap::Type SnapType) const
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!InstanceStack.IsValidIndex(InstanceIdx))
	{
		return;
	}

	const FBehaviorTreeInstance& ActiveInstance = InstanceStack[InstanceIdx];
	const FBehaviorTreeInstanceId& ActiveInstanceInfo = KnownInstances[ActiveInstance.InstanceIdIndex];
	InstanceInfo.TreeAsset = ActiveInstanceInfo.TreeAsset;
	InstanceInfo.RootNode = ActiveInstance.RootNode;

	if (SnapType == EBTExecutionSnap::Regular)
	{
		// traverse execution path
		UBTNode* StoreNode = ActiveInstance.ActiveNode ? ActiveInstance.ActiveNode : ActiveInstance.RootNode;
		while (StoreNode)
		{
			InstanceInfo.ActivePath.Add(StoreNode->GetExecutionIndex());
			StoreNode = StoreNode->GetParentNode();
		}

		// add aux nodes
		for (const UBTAuxiliaryNode* AuxNode : ActiveInstance.GetActiveAuxNodes())
		{
			InstanceInfo.AdditionalActiveNodes.Add(AuxNode->GetExecutionIndex());
		}

		// add active parallels
		for (const FBehaviorTreeParallelTask& TaskInfo : ActiveInstance.GetParallelTasks())
		{
			InstanceInfo.AdditionalActiveNodes.Add(TaskInfo.TaskNode->GetExecutionIndex());
		}

		// runtime values
		StoreDebuggerRuntimeValues(InstanceInfo.RuntimeDesc, ActiveInstance.RootNode, InstanceIdx);
	}

	// handle restart triggers
	if (CurrentRestarts.IsValidIndex(InstanceIdx))
	{
		InstanceInfo.PathFromPrevious = CurrentRestarts[InstanceIdx];
	}

	// store search flow, but remove nodes on execution path
	if (CurrentSearchFlow.IsValidIndex(InstanceIdx))
	{
		for (int32 FlowIndex = 0; FlowIndex < CurrentSearchFlow[InstanceIdx].Num(); FlowIndex++)
		{
			if (!InstanceInfo.ActivePath.Contains(CurrentSearchFlow[InstanceIdx][FlowIndex].ExecutionIndex))
			{
				InstanceInfo.PathFromPrevious.Add(CurrentSearchFlow[InstanceIdx][FlowIndex]);
			}
		}
	}
#endif
}

void UBehaviorTreeComponent::StoreDebuggerRemovedInstance(uint16 InstanceIdx) const
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!IsDebuggerActive())
	{
		return;
	}

	FBehaviorTreeDebuggerInstance StoreInfo;
	StoreDebuggerInstance(StoreInfo, InstanceIdx, EBTExecutionSnap::OutOfNodes);

	RemovedInstances.Add(StoreInfo);
#endif
}

void UBehaviorTreeComponent::StoreDebuggerSearchStep(const UBTNode* Node, uint16 InstanceIdx, EBTNodeResult::Type NodeResult) const
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!IsDebuggerActive())
	{
		return;
	}

	if (Node && NodeResult != EBTNodeResult::InProgress && NodeResult != EBTNodeResult::Aborted)
	{
		FBehaviorTreeDebuggerInstance::FNodeFlowData FlowInfo;
		FlowInfo.ExecutionIndex = Node->GetExecutionIndex();
		FlowInfo.bPassed = (NodeResult == EBTNodeResult::Succeeded);
		
		if (CurrentSearchFlow.Num() < (InstanceIdx + 1))
		{
			CurrentSearchFlow.SetNum(InstanceIdx + 1);
		}

		if (CurrentSearchFlow[InstanceIdx].Num() == 0 || CurrentSearchFlow[InstanceIdx].Last().ExecutionIndex != FlowInfo.ExecutionIndex)
		{
			CurrentSearchFlow[InstanceIdx].Add(FlowInfo);
		}
	}
#endif
}

void UBehaviorTreeComponent::StoreDebuggerSearchStep(const UBTNode* Node, uint16 InstanceIdx, bool bPassed) const
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!IsDebuggerActive())
	{
		return;
	}

	if (Node && !bPassed)
	{
		FBehaviorTreeDebuggerInstance::FNodeFlowData FlowInfo;
		FlowInfo.ExecutionIndex = Node->GetExecutionIndex();
		FlowInfo.bPassed = bPassed;

		if (CurrentSearchFlow.Num() < (InstanceIdx + 1))
		{
			CurrentSearchFlow.SetNum(InstanceIdx + 1);
		}

		CurrentSearchFlow[InstanceIdx].Add(FlowInfo);
	}
#endif
}

void UBehaviorTreeComponent::StoreDebuggerRestart(const UBTNode* Node, uint16 InstanceIdx, bool bAllowed)
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!IsDebuggerActive())
	{
		return;
	}

	if (Node)
	{
		FBehaviorTreeDebuggerInstance::FNodeFlowData FlowInfo;
		FlowInfo.ExecutionIndex = Node->GetExecutionIndex();
		FlowInfo.bTrigger = bAllowed;
		FlowInfo.bDiscardedTrigger = !bAllowed;

		if (CurrentRestarts.Num() < (InstanceIdx + 1))
		{
			CurrentRestarts.SetNum(InstanceIdx + 1);
		}

		CurrentRestarts[InstanceIdx].Add(FlowInfo);
	}
#endif
}

void UBehaviorTreeComponent::StoreDebuggerRuntimeValues(TArray<FString>& RuntimeDescriptions, UBTNode* RootNode, uint16 InstanceIdx) const
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!InstanceStack.IsValidIndex(InstanceIdx))
	{
		return;
	}

	const FBehaviorTreeInstance& InstanceInfo = InstanceStack[InstanceIdx];

	TArray<FString> RuntimeValues;
	for (UBTNode* Node = RootNode; Node; Node = Node->GetNextNode())
	{
		uint8* NodeMemory = (uint8*)Node->GetNodeMemory<uint8>(InstanceInfo);

		RuntimeValues.Reset();
		Node->DescribeRuntimeValues(*this, NodeMemory, EBTDescriptionVerbosity::Basic, RuntimeValues);

		FString ComposedDesc;
		for (int32 ValueIndex = 0; ValueIndex < RuntimeValues.Num(); ValueIndex++)
		{
			if (ComposedDesc.Len())
			{
				ComposedDesc.AppendChar(TEXT('\n'));
			}

			ComposedDesc += RuntimeValues[ValueIndex];
		}

		RuntimeDescriptions.SetNum(Node->GetExecutionIndex() + 1);
		RuntimeDescriptions[Node->GetExecutionIndex()] = ComposedDesc;
	}
#endif
}

void UBehaviorTreeComponent::UpdateDebuggerAfterExecution(const UBTTaskNode* TaskNode, uint16 InstanceIdx) const
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!IsDebuggerActive() || !InstanceStack.IsValidIndex(InstanceIdx))
	{
		return;
	}

	FBehaviorTreeExecutionStep& CurrentStep = DebuggerSteps.Last();

	// store runtime values
	TArray<FString> RuntimeValues;
	const FBehaviorTreeInstance& InstanceToUpdate = InstanceStack[InstanceIdx];
	uint8* NodeMemory = (uint8*)TaskNode->GetNodeMemory<uint8>(InstanceToUpdate);
	TaskNode->DescribeRuntimeValues(*this, NodeMemory, EBTDescriptionVerbosity::Basic, RuntimeValues);

	FString ComposedDesc;
	for (int32 ValueIndex = 0; ValueIndex < RuntimeValues.Num(); ValueIndex++)
	{
		if (ComposedDesc.Len())
		{
			ComposedDesc.AppendChar(TEXT('\n'));
		}

		ComposedDesc += RuntimeValues[ValueIndex];
	}

	// accessing RuntimeDesc should never be out of bounds (active task MUST be part of active instance)
	const uint16& ExecutionIndex = TaskNode->GetExecutionIndex();
	if (CurrentStep.InstanceStack[InstanceIdx].RuntimeDesc.IsValidIndex(ExecutionIndex))
	{
		CurrentStep.InstanceStack[InstanceIdx].RuntimeDesc[ExecutionIndex] = ComposedDesc;
	}
	else
	{
		UE_VLOG(GetOwner(), LogBehaviorTree, Error, TEXT("Incomplete debugger data! No runtime description for executed task, instance %d has only %d entries!"),
			InstanceIdx, CurrentStep.InstanceStack[InstanceIdx].RuntimeDesc.Num());
	}
#endif
}

void UBehaviorTreeComponent::StoreDebuggerBlackboard(TMap<FName, FString>& BlackboardValueDesc) const
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (!IsDebuggerActive())
	{
		return;
	}

	if (BlackboardComp && BlackboardComp->HasValidAsset())
	{
		const int32 NumKeys = BlackboardComp->GetNumKeys();
		BlackboardValueDesc.Empty(NumKeys);

		for (int32 KeyIndex = 0; KeyIndex < NumKeys; KeyIndex++)
		{
			FString Value = BlackboardComp->DescribeKeyValue(KeyIndex, EBlackboardDescription::OnlyValue);
			if (Value.Len() == 0)
			{
				Value = TEXT("n/a");
			}

			BlackboardValueDesc.Add(BlackboardComp->GetKeyName(KeyIndex), Value);
		}
	}
#endif
}

// Code for timing BT Search for FramePro
#if !UE_BUILD_SHIPPING
void UBehaviorTreeComponent::EndFrame()
{
	if (CVarBTRecordFrameSearchTimes.GetValueOnGameThread() != 0)
	{
		const double FrameSearchTimeMilliSecsDouble = FrameSearchTime * 1000.;
		const double AvFrameSearchTimeMilliSecsDouble = (NumSearchTimeCalls > 0) ? FrameSearchTimeMilliSecsDouble / static_cast<double>(NumSearchTimeCalls) : 0.;
		const float FrameSearchTimeMilliSecsFloat = static_cast<float>(FrameSearchTimeMilliSecsDouble);
		const float NumSearchTimeCallsFloat = static_cast<float>(NumSearchTimeCalls);
		const float AvFrameSearchTimeMilliSecsFloat = static_cast<float>(AvFrameSearchTimeMilliSecsDouble);

		FPlatformMisc::CustomNamedStat("BehaviorTreeSearchTimeFrameMs", FrameSearchTimeMilliSecsFloat, "BehaviorTree", "MilliSecs");
		FPlatformMisc::CustomNamedStat("BehaviorTreeSearchCallsFrame", NumSearchTimeCallsFloat, "BehaviorTree", "Count");
		FPlatformMisc::CustomNamedStat("BehaviorTreeSearchTimeFrameAvMs", AvFrameSearchTimeMilliSecsFloat, "BehaviorTree", "MilliSecs");

		FrameSearchTime = 0.;
		NumSearchTimeCalls = 0;
	}
}
#endif

bool UBehaviorTreeComponent::IsDebuggerActive()
{
#if USE_BEHAVIORTREE_DEBUGGER
	if (ActiveDebuggerCounter <= 0)
	{
		static bool bAlwaysGatherData = false;
		static uint64 PrevFrameCounter = 0;

		if (GFrameCounter != PrevFrameCounter)
		{
			GConfig->GetBool(TEXT("/Script/UnrealEd.EditorPerProjectUserSettings"), TEXT("bAlwaysGatherBehaviorTreeDebuggerData"), bAlwaysGatherData, GEditorPerProjectIni);
			PrevFrameCounter = GFrameCounter;
		}

		return bAlwaysGatherData;
	}

	return true;
#else
	return false;
#endif
}
