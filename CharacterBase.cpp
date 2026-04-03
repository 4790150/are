// Fill out your copyright notice in the Description page of Project Settings.


#include "CharacterBase.h"

#include "Car.h"
#include "ComponentUtils.h"
#include "VehicleMovementComponent.h"
#include "GameFramework/CharacterMovementComponent.h"


// Sets default values
ACharacterBase::ACharacterBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Set this character to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void ACharacterBase::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void ACharacterBase::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

// Called to bind functionality to input
void ACharacterBase::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
}

void ACharacterBase::MulticastDriveCar_Implementation(ACar* Car)
{
	if (UVehicleMovementComponent* VehicleMovementComponent = Car->GetComponentByClass<UVehicleMovementComponent>())
	{
		VehicleMovementComponent->SetDriver(this);
	}
}

void ACharacterBase::ServerDriveCar_Implementation(ACar* Car)
{
	MulticastDriveCar(Car);
}

void ACharacterBase::DriverServerMovePacked_Implementation(const FCharacterServerMovePackedBits& PackedBits)
{
	if (ACharacter* Car = Cast<ACharacter>(GetAttachParentActor()))
	{
		Car->GetCharacterMovement()->ServerMovePacked_ServerReceive(PackedBits);
	}
	else
	{
		GetCharacterMovement()->ServerMovePacked_ServerReceive(PackedBits);
	}
}
