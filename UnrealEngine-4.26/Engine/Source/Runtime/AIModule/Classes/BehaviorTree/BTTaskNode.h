// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "BehaviorTree/BTNode.h"
#include "BTTaskNode.generated.h"

class UBTService;

/** 
 * Task are leaf nodes of behavior tree, which perform actual actions
 *
 * Because some of them can be instanced for specific AI, following virtual functions are not marked as const:
 *  - ExecuteTask
 *  - AbortTask
 *  - TickTask
 *  - OnMessage
 *
 * If your node is not being instanced (default behavior), DO NOT change any properties of object within those functions!
 * Template nodes are shared across all behavior tree components using the same tree asset and must store
 * their runtime properties in provided NodeMemory block (allocation size determined by GetInstanceMemorySize() )
 *
 */

UCLASS(Abstract)
class AIMODULE_API UBTTaskNode : public UBTNode
{
	GENERATED_UCLASS_BODY()

	/** starts this task, should return Succeeded, Failed or InProgress
	 *  (use FinishLatentTask() when returning InProgress)
	 * this function should be considered as const (don't modify state of object) if node is not instanced! */
	// ��ʼ����Ӧ�÷���Succeeded��Failed����InProgress
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory);

protected:
	/** aborts this task, should return Aborted or InProgress
	 *  (use FinishLatentAbort() when returning InProgress)
	 * this function should be considered as const (don't modify state of object) if node is not instanced! */
	// ��ֹ����Ӧ�÷���Aborted����InProgress
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory);

public:
#if WITH_EDITOR
	virtual FName GetNodeIconName() const override;
#endif // WITH_EDITOR
	// ��Ϸ����ͣ��
	virtual void OnGameplayTaskDeactivated(UGameplayTask& Task) override;

	/** message observer's hook */
	// ��Ϣ�۲��ߵĹ���
	void ReceivedMessage(UBrainComponent* BrainComp, const FAIMessage& Message);

	/** wrapper for node instancing: ExecuteTask */
	// ʵ�����ڵ��װ������ExecuteTask
	EBTNodeResult::Type WrappedExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const;

	/** wrapper for node instancing: AbortTask */
	// ʵ�����ڵ��װ������AbortTask
	EBTNodeResult::Type WrappedAbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const;

	/** wrapper for node instancing: TickTask
	  * @param OwnerComp	The behavior tree owner of this node
	  * @param NodeMemory	The instance memory of the current node
	  * @param DeltaSeconds		DeltaTime since last call
	  * @param NextNeededDeltaTime		In out parameter, if this node needs a smaller DeltaTime it is his responsibility to change it
	  * @returns	True if it actually done some processing or false if it was skipped because of not ticking or in between time interval */
	// ʵ�����ڵ�İ�װ������TickTask
	bool WrappedTickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds, float& NextNeededDeltaTime) const;

	/** wrapper for node instancing: OnTaskFinished */
	// ʵ�����ڵ�İ�װ������OnTaskFinished
	void WrappedOnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) const;

	/** helper function: finish latent executing */
	// �������������Ǳ�ڵ�ִ��
	void FinishLatentTask(UBehaviorTreeComponent& OwnerComp, EBTNodeResult::Type TaskResult) const;

	/** helper function: finishes latent aborting */
	// �������������Ǳ�ڵ���ֹ
	void FinishLatentAbort(UBehaviorTreeComponent& OwnerComp) const;

	/** @return true if task search should be discarded when this task is selected to execute but is already running */
	// true��ʾ����������ѡ��ִ�е����Ѿ����������ˣ�����������Ӧ�ñ�����
	bool ShouldIgnoreRestartSelf() const;

	/** service nodes */
	// ����ڵ��б�
	UPROPERTY()
	TArray<UBTService*> Services;

protected:

	/** if set, task search will be discarded when this task is selected to execute but is already running */
	// ���ú󣬵�ѡ��ִ�е�������������ʱ������������������
	UPROPERTY(EditAnywhere, Category=Task)
	uint32 bIgnoreRestartSelf : 1;

	/** if set, TickTask will be called */
	// ������ã�TickTask���ᱻ����
	uint32 bNotifyTick : 1;

	/** if set, OnTaskFinished will be called */
	// ������ã�OnTaskFinished���ᱻ����
	uint32 bNotifyTaskFinished : 1;
	
	/** ticks this task 
	 * this function should be considered as const (don't modify state of object) if node is not instanced! */
	// ticks������
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds);

	/** message handler, default implementation will finish latent execution/abortion
	 * this function should be considered as const (don't modify state of object) if node is not instanced! */
	// ��Ϣ�ַ�����Ĭ��ʵ�ֽ����Ǳ�ڵ�ִ��/��ֹ
	virtual void OnMessage(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, FName Message, int32 RequestID, bool bSuccess);

	/** called when task execution is finished
	 * this function should be considered as const (don't modify state of object) if node is not instanced! */
	// ����ִ����ɺ����
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult);

	/** register message observer */
	// ע����Ϣ�۲���
	void WaitForMessage(UBehaviorTreeComponent& OwnerComp, FName MessageType) const;
	void WaitForMessage(UBehaviorTreeComponent& OwnerComp, FName MessageType, int32 RequestID) const;
	
	/** unregister message observers */
	// ע����Ϣ�۲���
	void StopWaitingForMessages(UBehaviorTreeComponent& OwnerComp) const;
};

FORCEINLINE bool UBTTaskNode::ShouldIgnoreRestartSelf() const
{
	return bIgnoreRestartSelf;
}
