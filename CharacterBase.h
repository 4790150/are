// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "CharacterBase.generated.h"

class ACar;

UCLASS()
class THIRDPERSON_API ACharacterBase : public ACharacter
{
	GENERATED_BODY()

public:
	// Sets default values for this character's properties
	ACharacterBase(const FObjectInitializer& ObjectInitializer);

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Called to bind functionality to input
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	
	UFUNCTION(BlueprintCallable, Server, Reliable)
	void ServerDriveCar(ACar* Car);
	
	UFUNCTION(NetMulticast, Reliable)
	void MulticastDriveCar(ACar* Car);
	
	UFUNCTION(unreliable, server)
	void DriverServerMovePacked(const FCharacterServerMovePackedBits& PackedBits);
	void DriverServerMovePacked_Implementation(const FCharacterServerMovePackedBits& PackedBits);
};
