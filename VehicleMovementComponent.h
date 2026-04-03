// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "VehicleMovementComponent.generated.h"


class ACharacterBase;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class THIRDPERSON_API UVehicleMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UVehicleMovementComponent(const FObjectInitializer& ObjectInitializer);

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

	virtual FVector ConsumeInputVector() override;
public:
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	
	virtual void ControlledCharacterMove(const FVector& InputVector, float DeltaSeconds) override;
	virtual void PerformMovement(float DeltaSeconds) override;
	virtual void ReplicateMoveToServer(float DeltaTime, const FVector& NewAcceleration) override;
	virtual void CallServerMovePacked(const FSavedMove_Character* NewMove, const FSavedMove_Character* PendingMove,
	                          const FSavedMove_Character* OldMove) override;
	virtual void CallServerMove(const FSavedMove_Character* NewMove, const FSavedMove_Character* OldMove) override;
	virtual void PhysWalking(float deltaTime, int32 Iterations) override;
	
	void SetDriver(ACharacterBase* NewDriver);
	
	ACharacterBase* GetDriver() const;
	
	UPROPERTY()
	ACharacterBase* Driver = nullptr;
	
	/** Used for writing server move RPC bits. */
	FNetBitWriter DriverServerMoveBitWriter;

	/** Used for reading server move RPC bits. */
	FNetBitReader DriverServerMoveBitReader;
};
