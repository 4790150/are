// Fill out your copyright notice in the Description page of Project Settings.


#include "VehicleMovementComponent.h"

#include "CharacterBase.h"
#include "GameFramework/Character.h"
#include "Net/Iris/ReplicationSystem/EngineReplicationBridge.h"
#include "Net/Iris/ReplicationSystem/ReplicationSystemUtil.h"

namespace CharacterMovementCVars
{
	static int32 VisualizeMovement = 0;
	FAutoConsoleVariableRef CVarVisualizeMovement(
		TEXT("p.VisualizeMovement"),
		VisualizeMovement,
		TEXT("Whether to draw in-world debug information for character movement.\n")
		TEXT("0: Disable, 1: Enable"),
		ECVF_Cheat);
}

namespace UE::Private
{
	static UIrisObjectReferencePackageMap* GetIrisPackageMapToCaptureReferences(UNetConnection* NetConnection, FCharacterNetworkSerializationPackedBits& PackedBits)
	{
		using namespace UE::Net;

		if (const UEngineReplicationBridge* Bridge = FReplicationSystemUtil::GetActorReplicationBridge(NetConnection))
		{
			if (UIrisObjectReferencePackageMap* ObjectReferencePackageMap = Bridge->GetObjectReferencePackageMap())
			{
				ObjectReferencePackageMap->InitForWrite(&PackedBits.PackageMapExports);

				return ObjectReferencePackageMap;
			}
		}

		return nullptr;
	}
}

// Sets default values for this component's properties
UVehicleMovementComponent::UVehicleMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	// PrimaryComponentTick.bCanEverTick = true;

	// ...
	DriverServerMoveBitWriter.SetAllowResize(true);
}


// Called when the game starts
void UVehicleMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	// ...
	
}

FVector UVehicleMovementComponent::ConsumeInputVector()
{
	if (IsValid(Driver))
	{
		return Driver->GetMovementComponent()->ConsumeInputVector();
	}
	
	return Super::ConsumeInputVector();
}


// Called every frame
void UVehicleMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                              FActorComponentTickFunction* ThisTickFunction)
{
	// SCOPED_NAMED_EVENT(UVehicleMovementComponent_TickComponent, FColor::Yellow);
	// SCOPE_CYCLE_COUNTER(STAT_VehicleMovement);
	// SCOPE_CYCLE_COUNTER(STAT_VehicleMovementTick);
	// CSV_SCOPED_TIMING_STAT_EXCLUSIVE(VehicleMovement);

	// Do not consume input if simulating asynchronously, we will consume input when filling out async inputs.
	FVector InputVector = ConsumeInputVector();

	if (!HasValidData() || ShouldSkipUpdate(DeltaTime))
	{
		return;
	}

	UPawnMovementComponent::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Super tick may destroy/invalidate CharacterOwner or UpdatedComponent, so we need to re-check.
	if (!HasValidData())
	{
		return;
	}

	AvoidanceLockTimer -= DeltaTime;

	if (GetDriver()->GetLocalRole() > ROLE_SimulatedProxy)
	{
		// SCOPE_CYCLE_COUNTER(STAT_VehicleMovementNonSimulated);

		// If we are a client we might have received an update from the server.
		const bool bIsClient = (GetDriver()->GetLocalRole() == ROLE_AutonomousProxy && IsNetMode(NM_Client));
		if (bIsClient)
		{
			FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
			if (ClientData && ClientData->bUpdatePosition)
			{
				ClientUpdatePositionAfterServerUpdate();
			}
		}

		// Defer all mesh child updates until all movement completes. Avoiding combining this with the corrections above, as those may cause a lot of significant movement.
		// FScopedMeshMovementUpdate ScopedMeshUpdate(CharacterOwner->GetMesh());

		// Perform input-driven move for any locally-controlled character, and also
		// allow animation root motion or physics to move characters even if they have no controller
		const bool bShouldPerformControlledCharMove = GetDriver()->IsLocallyControlled() 
													  || (!GetDriver()->GetController() && bRunPhysicsWithNoController)		
													  || (!CharacterOwner->GetController() && CharacterOwner->IsPlayingRootMotion());

		if (bShouldPerformControlledCharMove)
		{
			ControlledCharacterMove(InputVector, DeltaTime);

			const bool bIsaListenServerAutonomousProxy = GetDriver()->IsLocallyControlled()
													 	 && (GetDriver()->GetRemoteRole() == ROLE_AutonomousProxy);

			if (bIsaListenServerAutonomousProxy)
			{
				ServerAutonomousProxyTick(DeltaTime);
			}
		}
		else if (GetDriver()->GetRemoteRole() == ROLE_AutonomousProxy)
		{
			// Server ticking for remote client.
			// Between net updates from the client we need to update position if based on another object,
			// otherwise the object will move on intermediate frames and we won't follow it.
			MaybeUpdateBasedMovement(DeltaTime);
			MaybeSaveBaseLocation();

			ServerAutonomousProxyTick(DeltaTime);

			// Smooth on listen server for local view of remote clients. We may receive updates at a rate different than our own tick rate.
			if (/*CharacterMovementCVars::NetEnableListenServerSmoothing &&*/ !bNetworkSmoothingComplete && IsNetMode(NM_ListenServer))
			{
				SmoothClientPosition(DeltaTime);
			}
		}
	}
	else if (GetDriver()->GetLocalRole() == ROLE_SimulatedProxy)
	{
		// Defer all mesh child updates until all movement completes.
		// FScopedMeshMovementUpdate ScopedMeshUpdate(CharacterOwner->GetMesh());

		if (bShrinkProxyCapsule)
		{
			AdjustProxyCapsuleSize();
		}
		SimulatedTick(DeltaTime);
	}

	if (bUseRVOAvoidance)
	{
		UpdateDefaultAvoidance();
	}

	if (bEnablePhysicsInteraction)
	{
		// SCOPE_CYCLE_COUNTER(STAT_CharPhysicsInteraction);
		ApplyDownwardForce(DeltaTime);
		ApplyRepulsionForce(DeltaTime);
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const bool bVisualizeMovement = CharacterMovementCVars::VisualizeMovement > 0;
	if (bVisualizeMovement)
	{
		VisualizeMovement();
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

}

void UVehicleMovementComponent::ControlledCharacterMove(const FVector& InputVector, float DeltaSeconds)
{
	{
		// SCOPE_CYCLE_COUNTER(STAT_CharUpdateAcceleration);

		// We need to check the jump state before adjusting input acceleration, to minimize latency
		// and to make sure acceleration respects our potentially new falling state.
		CharacterOwner->CheckJumpInput(DeltaSeconds);

		// apply input to acceleration
		Acceleration = ScaleInputAcceleration(ConstrainInputAcceleration(InputVector));
		AnalogInputModifier = ComputeAnalogInputModifier();
	}

	if (GetDriver()->GetLocalRole() == ROLE_Authority)
	{
		PerformMovement(DeltaSeconds);
	}
	else if (GetDriver()->GetLocalRole() == ROLE_AutonomousProxy && IsNetMode(NM_Client))
	{
		ReplicateMoveToServer(DeltaSeconds, Acceleration);
	}
}

void UVehicleMovementComponent::PerformMovement(float DeltaSeconds)
{
	const UWorld* MyWorld = GetWorld();
	if (!HasValidData() || MyWorld == nullptr)
	{
		return;
	}

	bTeleportedSinceLastUpdate = UpdatedComponent->GetComponentLocation() != LastUpdateLocation;

	// Force floor update if we've moved outside of CharacterMovement since last update.
	bForceNextFloorCheck |= (IsMovingOnGround() && bTeleportedSinceLastUpdate);

	FVector OldVelocity;
	FVector OldLocation;

	// Scoped updates can improve performance of multiple MoveComponent calls.
	{
		MaybeUpdateBasedMovement(DeltaSeconds);
		
		OldVelocity = Velocity;
		OldLocation = UpdatedComponent->GetComponentLocation();

		ApplyAccumulatedForces(DeltaSeconds);

		// Update the character state before we do our movement
		UpdateCharacterStateBeforeMovement(DeltaSeconds);

		if (MovementMode == MOVE_NavWalking && bWantsToLeaveNavWalking)
		{
			TryToLeaveNavWalking();
		}

		// Character::LaunchCharacter() has been deferred until now.
		HandlePendingLaunch();
		ClearAccumulatedForces();

		// NaN tracking
		// devCode(ensureMsgf(!Velocity.ContainsNaN(), TEXT("UCharacterMovementComponent::PerformMovement: Velocity contains NaN (%s)\n%s"), *GetPathNameSafe(this), *Velocity.ToString()));

		// Clear jump input now, to allow movement events to trigger it for next update.
		CharacterOwner->ClearJumpInput(DeltaSeconds);
		NumJumpApexAttempts = 0;

		// Give one final chance to update the velocity right before doing the actual position change.
		UpdateVelocityBeforeMovement(DeltaSeconds);
		
		// change position
		StartNewPhysics(DeltaSeconds, 0);

		if (!HasValidData())
		{
			return;
		}

		// Update character state based on change from movement
		UpdateCharacterStateAfterMovement(DeltaSeconds);

		if (bAllowPhysicsRotationDuringAnimRootMotion || !HasAnimRootMotion())
		{
			PhysicsRotation(DeltaSeconds);
		}

		// consume path following requested velocity
		LastUpdateRequestedVelocity = bHasRequestedVelocity ? RequestedVelocity : FVector::ZeroVector;
		bHasRequestedVelocity = false;

		OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);
	} // End scoped movement update

	// Call external post-movement events. These happen after the scoped movement completes in case the events want to use the current state of overlaps etc.
	CallMovementUpdateDelegate(DeltaSeconds, OldLocation, OldVelocity);

	// if (CharacterMovementCVars::BasedMovementMode == 0)
	// {
	// 	SaveBaseLocation(); // behaviour before implementing this fix
	// }
	// else
	{
		MaybeSaveBaseLocation();
	}
	UpdateComponentVelocity();

	const bool bHasAuthority = CharacterOwner && CharacterOwner->HasAuthority();

	// If we move we want to avoid a long delay before replication catches up to notice this change, especially if it's throttling our rate.
	if (bHasAuthority && UNetDriver::IsAdaptiveNetUpdateFrequencyEnabled() && UpdatedComponent)
	{
		UNetDriver* NetDriver = MyWorld->GetNetDriver();
		if (NetDriver && NetDriver->IsServer())
		{
			if (!NetDriver->IsPendingNetUpdate(CharacterOwner) && NetDriver->IsNetworkActorUpdateFrequencyThrottled(CharacterOwner))
			{
				if (ShouldCancelAdaptiveReplication())
				{
					NetDriver->CancelAdaptiveReplication(CharacterOwner);
				}
			}
		}
	}

	const FVector NewLocation = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
	const FQuat NewRotation = UpdatedComponent ? UpdatedComponent->GetComponentQuat() : FQuat::Identity;

	if (bHasAuthority && UpdatedComponent && !IsNetMode(NM_Client))
	{
		const bool bLocationChanged = (NewLocation != LastUpdateLocation);
		const bool bRotationChanged = (NewRotation != LastUpdateRotation);
		if (bLocationChanged || bRotationChanged)
		{
			// Update ServerLastTransformUpdateTimeStamp. This is used by Linear smoothing on clients to interpolate positions with the correct delta time,
			// so the timestamp should be based on the client's move delta (ServerAccumulatedClientTimeStamp), not the server time when receiving the RPC.
			const bool bIsRemotePlayer = (CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy);
			const FNetworkPredictionData_Server_Character* ServerData = bIsRemotePlayer ? GetPredictionData_Server_Character() : nullptr;
			if (bIsRemotePlayer && ServerData /* && CharacterMovementCVars::NetUseClientTimestampForReplicatedTransform */)
			{
				ServerLastTransformUpdateTimeStamp = float(ServerData->ServerAccumulatedClientTimeStamp);
			}
			else
			{
				ServerLastTransformUpdateTimeStamp = MyWorld->GetTimeSeconds();
			}
		}
	}

	LastUpdateLocation = NewLocation;
	LastUpdateRotation = NewRotation;
	LastUpdateVelocity = Velocity;
}

void UVehicleMovementComponent::ReplicateMoveToServer(float DeltaTime, const FVector& NewAcceleration)
{
	// Can only start sending moves if our controllers are synced up over the network, otherwise we flood the reliable buffer.
	APlayerController* PC = Cast<APlayerController>(GetDriver()->GetController());
	if (PC && PC->AcknowledgedPawn != GetDriver())
	{
		return;
	}

	// Bail out if our character's controller doesn't have a Player. This may be the case when the local player
	// has switched to another controller, such as a debug camera controller.
	if (PC && PC->Player == nullptr)
	{
		return;
	}

	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	if (!ClientData)
	{
		return;
	}
	
	// Update our delta time for physics simulation.
	DeltaTime = ClientData->UpdateTimeStampAndDeltaTime(DeltaTime, *GetDriver(), *this);

	// Find the oldest (unacknowledged) important move (OldMove).
	// Don't include the last move because it may be combined with the next new move.
	// A saved move is interesting if it differs significantly from the last acknowledged move
	FSavedMovePtr OldMove = NULL;
	if( ClientData->LastAckedMove.IsValid() )
	{
		const int32 NumSavedMoves = ClientData->SavedMoves.Num();
		for (int32 i=0; i < NumSavedMoves-1; i++)
		{
			const FSavedMovePtr& CurrentMove = ClientData->SavedMoves[i];
			if (CurrentMove->IsImportantMove(ClientData->LastAckedMove))
			{
				OldMove = CurrentMove;
				break;
			}
		}
	}

	// Get a SavedMove object to store the movement in.
	FSavedMovePtr NewMovePtr = ClientData->CreateSavedMove();
	FSavedMove_Character* const NewMove = NewMovePtr.Get();
	if (NewMove == nullptr)
	{
		return;
	}

	NewMove->SetMoveFor(GetDriver(), DeltaTime, NewAcceleration, *ClientData);
	const UWorld* MyWorld = GetWorld();

	// see if the two moves could be combined
	// do not combine moves which have different TimeStamps (before and after reset).
	if (const FSavedMove_Character* PendingMove = ClientData->PendingMove.Get())
	{
		if (PendingMove->CanCombineWith(NewMovePtr, GetDriver(), ClientData->MaxMoveDeltaTime * GetDriver()->GetActorTimeDilation(*MyWorld)))
		{
			// Only combine and move back to the start location if we don't move back in to a spot that would make us collide with something new.
			const FVector OldStartLocation = PendingMove->GetRevertedLocation();
			const bool bAttachedToObject = (NewMovePtr->StartAttachParent != nullptr);
			if (bAttachedToObject || !OverlapTest(OldStartLocation, PendingMove->StartRotation.Quaternion(), UpdatedComponent->GetCollisionObjectType(), GetPawnCapsuleCollisionShape(SHRINK_None), CharacterOwner))
			{
				NewMove->CombineWith(PendingMove, GetDriver(), PC, OldStartLocation);

				if (PC)
				{
					// We reverted position to that at the start of the pending move (above), however some code paths expect rotation to be set correctly
					// before character movement occurs (via FaceRotation), so try that now. The bOrientRotationToMovement path happens later as part of PerformMovement() and PhysicsRotation().
					CharacterOwner->FaceRotation(PC->GetControlRotation(), NewMove->DeltaTime);
				}

				SaveBaseLocation();
				NewMove->SetInitialPosition(CharacterOwner);

				// Remove pending move from move list. It would have to be the last move on the list.
				if (ClientData->SavedMoves.Num() > 0 && ClientData->SavedMoves.Last() == ClientData->PendingMove)
				{
					ClientData->SavedMoves.Pop(EAllowShrinking::No);
				}
				ClientData->FreeMove(ClientData->PendingMove);
				ClientData->PendingMove = nullptr;
				PendingMove = nullptr; // Avoid dangling reference, it's deleted above.
			}
			else
			{
				UE_LOG(LogNetPlayerMovement, Verbose, TEXT("Not combining move [would collide at start location]"));
			}
		}
		else
		{
			UE_LOG(LogNetPlayerMovement, Verbose, TEXT("Not combining move [not allowed by CanCombineWith()]"));
		}
	}

	// Acceleration should match what we send to the server, plus any other restrictions the server also enforces (see MoveAutonomous).
	Acceleration = NewMove->Acceleration.GetClampedToMaxSize(GetMaxAcceleration());
	AnalogInputModifier = ComputeAnalogInputModifier(); // recompute since acceleration may have changed.

	// Perform the move locally
	CharacterOwner->ClientRootMotionParams.Clear();
	CharacterOwner->SavedRootMotion.Clear();
	PerformMovement(NewMove->DeltaTime);

	NewMove->PostUpdate(GetDriver(), FSavedMove_Character::PostUpdate_Record);

	// Add NewMove to the list
	if (GetDriver()->IsReplicatingMovement())
	{
		check(NewMove == NewMovePtr.Get());
		ClientData->SavedMoves.Push(NewMovePtr);

		const bool bCanDelayMove = CanDelaySendingMove(NewMovePtr);
		
		if (bCanDelayMove && ClientData->PendingMove.IsValid() == false)
		{
			// Decide whether to hold off on move
			const float NetMoveDeltaSeconds = FMath::Clamp(GetClientNetSendDeltaTime(PC, ClientData, NewMovePtr), 1.f/120.f, 1.f/5.f);
			const float SecondsSinceLastMoveSent = MyWorld->GetRealTimeSeconds() - ClientData->ClientUpdateRealTime;

			if (SecondsSinceLastMoveSent < NetMoveDeltaSeconds)
			{
				// Delay sending this move.
				ClientData->PendingMove = NewMovePtr;
				return;
			}
		}

		ClientData->ClientUpdateRealTime = MyWorld->GetRealTimeSeconds();

		UE_CLOG(CharacterOwner && UpdatedComponent, LogNetPlayerMovement, VeryVerbose, TEXT("ClientMove Time %f Acceleration %s Velocity %s Position %s Rotation %s DeltaTime %f Mode %s MovementBase %s.%s (Dynamic:%d) DualMove? %d"),
			NewMove->TimeStamp, *NewMove->Acceleration.ToString(), *Velocity.ToString(), *UpdatedComponent->GetComponentLocation().ToString(), *UpdatedComponent->GetComponentRotation().ToCompactString(), NewMove->DeltaTime, *GetMovementName(),
			*GetNameSafe(NewMove->EndBase.Get()), *NewMove->EndBoneName.ToString(), MovementBaseUtility::IsDynamicBase(NewMove->EndBase.Get()) ? 1 : 0, ClientData->PendingMove.IsValid() ? 1 : 0);

		
		bool bSendServerMove = true;

		// Send move to server if this character is replicating movement
		if (bSendServerMove)
		{
			if (ShouldUsePackedMovementRPCs())
			{
				CallServerMovePacked(NewMove, ClientData->PendingMove.Get(), OldMove.Get());
			}
			else
			{
				CallServerMove(NewMove, OldMove.Get());
			}
		}
	}

	ClientData->PendingMove = NULL;
}

void UVehicleMovementComponent::CallServerMovePacked(const FSavedMove_Character* NewMove, const FSavedMove_Character* PendingMove, const FSavedMove_Character* OldMove)
{
	// Get storage container we'll be using and fill it with movement data
	FCharacterNetworkMoveDataContainer& MoveDataContainer = GetNetworkMoveDataContainer();
	MoveDataContainer.ClientFillNetworkMoveData(NewMove, PendingMove, OldMove);

	// Reset bit writer without affecting allocations
	FBitWriterMark BitWriterReset;
	BitWriterReset.Pop(DriverServerMoveBitWriter);

	// 'static' to avoid reallocation each invocation
	static FCharacterServerMovePackedBits PackedBits;
	UNetConnection* NetConnection = GetDriver()->GetNetConnection();	

	if (UPackageMap* PackageMap = UE::Private::GetIrisPackageMapToCaptureReferences(NetConnection, PackedBits))
	{
		DriverServerMoveBitWriter.PackageMap = PackageMap;
	}
	else
	{
		// Extract the net package map used for serializing object references.
		DriverServerMoveBitWriter.PackageMap = NetConnection ? ToRawPtr(NetConnection->PackageMap) : nullptr;
	}

	if (DriverServerMoveBitWriter.PackageMap == nullptr)
	{
		UE_LOG(LogNetPlayerMovement, Error, TEXT("CallServerMovePacked: Failed to find a NetConnection/PackageMap for data serialization!"));
		return;
	}

	// Reset captured exports stored in PackedBits
	PackedBits.NetTokensPendingExport.Reset();
	UE::Net::FNetTokenExportScope NetTokenExportScope(DriverServerMoveBitWriter, NetConnection->GetDriver()->GetNetTokenStore(), PackedBits.NetTokensPendingExport, "CallServerMovePacked");

	// Serialize move struct into a bit stream
	if (!MoveDataContainer.Serialize(*this, DriverServerMoveBitWriter, DriverServerMoveBitWriter.PackageMap) || DriverServerMoveBitWriter.IsError())
	{
		UE_LOG(LogNetPlayerMovement, Error, TEXT("CallServerMovePacked: Failed to serialize out movement data!"));
		return;
	}

	// Copy bits to our struct that we can NetSerialize to the server.
	PackedBits.DataBits.SetNumUninitialized(DriverServerMoveBitWriter.GetNumBits());
	
	check(PackedBits.DataBits.Num() >= DriverServerMoveBitWriter.GetNumBits());
	FMemory::Memcpy(PackedBits.DataBits.GetData(), DriverServerMoveBitWriter.GetData(), DriverServerMoveBitWriter.GetNumBytes());

	// Send bits to server!
	// ServerMovePacked_ClientSend(PackedBits);
	GetDriver()->DriverServerMovePacked(PackedBits);
 
	MarkForClientCameraUpdate();
}


///// DEPRECATED /////
void UVehicleMovementComponent::CallServerMove
	(
	const FSavedMove_Character* NewMove,
	const FSavedMove_Character* OldMove
	)
{
	check(NewMove != nullptr);

	// Compress rotation down to 5 bytes
	uint32 ClientYawPitchINT = 0;
	uint8 ClientRollBYTE = 0;
	NewMove->GetPackedAngles(ClientYawPitchINT, ClientRollBYTE);

	// Determine if we send absolute or relative location
	UPrimitiveComponent* ClientMovementBase = NewMove->EndBase.Get();
	const FName ClientBaseBone = NewMove->EndBoneName;
	const FVector SendLocation = MovementBaseUtility::UseRelativeLocation(ClientMovementBase) ? NewMove->SavedRelativeLocation : FRepMovement::RebaseOntoZeroOrigin(NewMove->SavedLocation, this);

	// send old move if it exists
	if (OldMove)
	{
		ServerMoveOld(OldMove->TimeStamp, OldMove->Acceleration, OldMove->GetCompressedFlags());
	}

	FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
	if (const FSavedMove_Character* const PendingMove = ClientData->PendingMove.Get())
	{
		uint32 OldClientYawPitchINT = 0;
		uint8 OldClientRollBYTE = 0;
		ClientData->PendingMove->GetPackedAngles(OldClientYawPitchINT, OldClientRollBYTE);

		// If we delayed a move without root motion, and our new move has root motion, send these through a special function, so the server knows how to process them.
		if ((PendingMove->RootMotionMontage == NULL) && (NewMove->RootMotionMontage != NULL))
		{
			// send two moves simultaneously
			ServerMoveDualHybridRootMotion(
				PendingMove->TimeStamp,
				PendingMove->Acceleration,
				PendingMove->GetCompressedFlags(),
				OldClientYawPitchINT,
				NewMove->TimeStamp,
				NewMove->Acceleration,
				SendLocation,
				NewMove->GetCompressedFlags(),
				ClientRollBYTE,
				ClientYawPitchINT,
				ClientMovementBase,
				ClientBaseBone,
				NewMove->EndPackedMovementMode
				);
		}
		else
		{
			// send two moves simultaneously
			ServerMoveDual(
				PendingMove->TimeStamp,
				PendingMove->Acceleration,
				PendingMove->GetCompressedFlags(),
				OldClientYawPitchINT,
				NewMove->TimeStamp,
				NewMove->Acceleration,
				SendLocation,
				NewMove->GetCompressedFlags(),
				ClientRollBYTE,
				ClientYawPitchINT,
				ClientMovementBase,
				ClientBaseBone,
				NewMove->EndPackedMovementMode
				);
		}
	}
	else
	{
		ServerMove(
			NewMove->TimeStamp,
			NewMove->Acceleration,
			SendLocation,
			NewMove->GetCompressedFlags(),
			ClientRollBYTE,
			ClientYawPitchINT,
			ClientMovementBase,
			ClientBaseBone,
			NewMove->EndPackedMovementMode
			);
	}

	MarkForClientCameraUpdate();
}

void UVehicleMovementComponent::PhysWalking(float deltaTime, int32 Iterations)
{
	if (deltaTime < MIN_TICK_TIME)
	{
		return;
	}

	if (!GetDriver() || (!GetDriver()->GetController() && !bRunPhysicsWithNoController && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && (GetDriver()->GetLocalRole() != ROLE_SimulatedProxy)))
	{
		Acceleration = FVector::ZeroVector;
		Velocity = FVector::ZeroVector;
		return;
	}

	if (!UpdatedComponent->IsQueryCollisionEnabled())
	{
		SetMovementMode(MOVE_Walking);
		return;
	}
	
	bJustTeleported = false;
	bool bCheckedFall = false;
	bool bTriedLedgeMove = false;
	float remainingTime = deltaTime;

	const EMovementMode StartingMovementMode = MovementMode;
	const uint8 StartingCustomMovementMode = CustomMovementMode;

	// Perform the move
	while ( (remainingTime >= MIN_TICK_TIME) && (Iterations < MaxSimulationIterations) && GetDriver() && (GetDriver()->GetController() || bRunPhysicsWithNoController || HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity() || (GetDriver()->GetLocalRole() == ROLE_SimulatedProxy)) )
	{
		Iterations++;
		bJustTeleported = false;
		const float timeTick = GetSimulationTimeStep(remainingTime, Iterations);
		remainingTime -= timeTick;
		
		// Save current values
		UPrimitiveComponent * const OldBase = GetMovementBase();
		const FVector PreviousBaseLocation = (OldBase != NULL) ? OldBase->GetComponentLocation() : FVector::ZeroVector;
		const FVector OldLocation = UpdatedComponent->GetComponentLocation();
		const FFindFloorResult OldFloor = CurrentFloor;

		// Ensure velocity is horizontal.
		MaintainHorizontalGroundVelocity();
		const FVector OldVelocity = Velocity;
		Acceleration = FVector::VectorPlaneProject(Acceleration, -GetGravityDirection());

		// Apply acceleration
		const bool bSkipForLedgeMove = bTriedLedgeMove;
		if( !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && !bSkipForLedgeMove )
		{
			CalcVelocity(timeTick, GroundFriction, false, GetMaxBrakingDeceleration());
		}

		if (MovementMode != StartingMovementMode || CustomMovementMode != StartingCustomMovementMode)
		{
			// Root motion could have taken us out of our current mode
			// No movement has taken place this movement tick so we pass on full time/past iteration count
			StartNewPhysics(remainingTime+timeTick, Iterations-1);
			return;
		}

		// Compute move parameters
		const FVector MoveVelocity = Velocity;
		const FVector Delta = timeTick * MoveVelocity;
		const bool bZeroDelta = Delta.IsNearlyZero();
		FStepDownResult StepDownResult;

		if ( bZeroDelta )
		{
			remainingTime = 0.f;
		}
		else
		{
			// try to move forward
			MoveAlongFloor(MoveVelocity, timeTick, &StepDownResult);

			if (IsSwimming()) //just entered water
			{
				StartSwimming(OldLocation, OldVelocity, timeTick, remainingTime, Iterations);
				return;
			}
			else if (MovementMode != StartingMovementMode || CustomMovementMode != StartingCustomMovementMode)
			{
				// pawn ended up in a different mode, probably due to the step-up-and-over flow
				// let's refund the estimated unused time (if any) and keep moving in the new mode
				const float DesiredDist = Delta.Size();
				if (DesiredDist > UE_KINDA_SMALL_NUMBER)
				{
					const float ActualDist = ProjectToGravityFloor(UpdatedComponent->GetComponentLocation() - OldLocation).Size();
					remainingTime += timeTick * (1.f - FMath::Min(1.f,ActualDist/DesiredDist));
				}
				StartNewPhysics(remainingTime,Iterations);
				return;
			}
		}

		// Update floor.
		// StepUp might have already done it for us.
		if (StepDownResult.bComputedFloor)
		{
			CurrentFloor = StepDownResult.FloorResult;
		}
		else
		{
			FindFloor(UpdatedComponent->GetComponentLocation(), CurrentFloor, bZeroDelta, NULL);
		}

		// check for ledges here
		const bool bCheckLedges = !CanWalkOffLedges();
		if ( bCheckLedges && !CurrentFloor.IsWalkableFloor() )
		{
			// calculate possible alternate movement
			const FVector NewDelta = bTriedLedgeMove ? FVector::ZeroVector : GetLedgeMove(OldLocation, Delta, OldFloor);
			if ( !NewDelta.IsZero() )
			{
				// first revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, false);

				// avoid repeated ledge moves if the first one fails
				bTriedLedgeMove = true;

				// Try new movement direction
				Velocity = NewDelta/timeTick;
				remainingTime += timeTick;
				Iterations--;
				continue;
			}
			else
			{
				// see if it is OK to jump
				// @todo collision : only thing that can be problem is that oldbase has world collision on
				bool bMustJump = bZeroDelta || (OldBase == NULL || (!OldBase->IsQueryCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ( (bMustJump || !bCheckedFall) && CheckFall(OldFloor, CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump) )
				{
					return;
				}
				bCheckedFall = true;

				// revert this move
				RevertMove(OldLocation, OldBase, PreviousBaseLocation, OldFloor, true);
				remainingTime = 0.f;
				break;
			}
		}
		else
		{
			// Validate the floor check
			if (CurrentFloor.IsWalkableFloor())
			{
				if (ShouldCatchAir(OldFloor, CurrentFloor))
				{
					HandleWalkingOffLedge(OldFloor.HitResult.ImpactNormal, OldFloor.HitResult.Normal, OldLocation, timeTick);
					if (IsMovingOnGround())
					{
						// If still walking, then fall. If not, assume the user set a different mode they want to keep.
						StartFalling(Iterations, remainingTime, timeTick, Delta, OldLocation);
					}
					return;
				}

				AdjustFloorHeight();
				SetBaseFromFloor(CurrentFloor);
			}
			else if (CurrentFloor.HitResult.bStartPenetrating && remainingTime <= 0.f)
			{
				// The floor check failed because it started in penetration
				// We do not want to try to move downward because the downward sweep failed, rather we'd like to try to pop out of the floor.
				FHitResult Hit(CurrentFloor.HitResult);
				Hit.TraceEnd = Hit.TraceStart + MAX_FLOOR_DIST * -GetGravityDirection();
				const FVector RequestedAdjustment = GetPenetrationAdjustment(Hit);
				ResolvePenetration(RequestedAdjustment, Hit, UpdatedComponent->GetComponentQuat());
				bForceNextFloorCheck = true;
			}

			// check if just entered water
			if ( IsSwimming() )
			{
				StartSwimming(OldLocation, Velocity, timeTick, remainingTime, Iterations);
				return;
			}

			// See if we need to start falling.
			if (!CurrentFloor.IsWalkableFloor() && !CurrentFloor.HitResult.bStartPenetrating)
			{
				const bool bMustJump = bJustTeleported || bZeroDelta || (OldBase == NULL || (!OldBase->IsQueryCollisionEnabled() && MovementBaseUtility::IsDynamicBase(OldBase)));
				if ((bMustJump || !bCheckedFall) && CheckFall(OldFloor, CurrentFloor.HitResult, Delta, OldLocation, remainingTime, timeTick, Iterations, bMustJump) )
				{
					return;
				}
				bCheckedFall = true;
			}
		}


		// Allow overlap events and such to change physics state and velocity
		if (IsMovingOnGround())
		{
			// Make velocity reflect actual move
			if( !bJustTeleported && !HasAnimRootMotion() && !CurrentRootMotion.HasOverrideVelocity() && timeTick >= MIN_TICK_TIME)
			{
				// TODO-RootMotionSource: Allow this to happen during partial override Velocity, but only set allowed axes?
				Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation) / timeTick;
				MaintainHorizontalGroundVelocity();
			}
		}

		// If we didn't move at all this iteration then abort (since future iterations will also be stuck).
		if (UpdatedComponent->GetComponentLocation() == OldLocation)
		{
			remainingTime = 0.f;
			break;
		}	
	}

	if (IsMovingOnGround())
	{
		MaintainHorizontalGroundVelocity();
	}
}

void UVehicleMovementComponent::SetDriver(ACharacterBase* NewDriver)
{
	if (NewDriver == Driver)
		return;
	
	if (IsValid(Driver))
	{
		Driver->DetachFromActor(FDetachmentTransformRules::KeepRelativeTransform);
		Driver->SetActorEnableCollision(true);
		Driver->GetCharacterMovement()->SetComponentTickEnabled(true);
	}
	
	if (IsValid(NewDriver))
	{
		NewDriver->AttachToActor(GetOwner(), FAttachmentTransformRules::KeepRelativeTransform);
		NewDriver->SetActorEnableCollision(false);
		NewDriver->GetCharacterMovement()->SetComponentTickEnabled(false);
	}
	Driver = NewDriver;
}

ACharacterBase* UVehicleMovementComponent::GetDriver() const
{
	if (IsValid(Driver))
	{
		return Driver;
	}
	
	return Cast<ACharacterBase>(CharacterOwner);
}
