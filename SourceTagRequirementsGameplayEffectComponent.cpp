#include "SourceTagRequirementsGameplayEffectComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemLog.h"
#include "Algo/Find.h"
#include "Misc/DataValidation.h"
#include "GameplayAbilities/Private/AbilitySystemPrivate.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SourceTagRequirementsGameplayEffectComponent)

#define LOCTEXT_NAMESPACE "SourceTagRequirementsGameplayEffectComponent"

USourceTagRequirementsGameplayEffectComponent::USourceTagRequirementsGameplayEffectComponent()
{
#if WITH_EDITORONLY_DATA
	EditorFriendlyName = TEXT("Source Tag Reqs (While GE is Active)");
#endif // WITH_EDITORONLY_DATA
}

bool USourceTagRequirementsGameplayEffectComponent::CanGameplayEffectApply(const FActiveGameplayEffectsContainer& ActiveGEContainer, const FGameplayEffectSpec& GESpec) const
{
	FGameplayTagContainer Tags;
	GESpec.GetEffectContext().GetInstigatorAbilitySystemComponent()->GetOwnedGameplayTags(Tags);
	
	if (ApplicationTagRequirements.RequirementsMet(Tags) == false)
	{
		return false;
	}

	if (HaveRemovalRequirementsBeenMet(Tags, ActiveGEContainer.IsNetAuthority()))
	{
		return false;
	}

	return true;
}

// USourceTagRequirementsGameplayEffectComponent lives on an asset.  This doesn't get instanced at runtime, so this is NOT A UNIQUE INSTANCE (it is a shared instance for any GEContainer/ActiveGE that wants to use it).
bool USourceTagRequirementsGameplayEffectComponent::OnActiveGameplayEffectAdded(FActiveGameplayEffectsContainer& GEContainer, FActiveGameplayEffect& ActiveGE) const
{
	UAbilitySystemComponent* SourceASC = ActiveGE.Spec.GetEffectContext().GetInstigatorAbilitySystemComponent();
	if (!ensure(SourceASC))
	{
		return false;
	}
	
	UAbilitySystemComponent* TargetASC = GEContainer.Owner;
	if (!ensure(TargetASC))
	{
		return false;
	}

	using namespace UE::AbilitySystem::Private;
	const bool bAllowClientSideRemoval = EnumHasAnyFlags(static_cast<EAllowPredictiveGEFlags>(CVarAllowPredictiveGEFlagsValue), EAllowPredictiveGEFlags::AllowRemovalByTagRequirements);
	const bool bAuthority = GEContainer.IsNetAuthority();

	FActiveGameplayEffectHandle ActiveGEHandle = ActiveGE.Handle;
	if (FActiveGameplayEffectEvents* EventSet = TargetASC->GetActiveEffectEventSet(ActiveGEHandle))
	{
		// Quick method of appending a TArray to another TArray with no duplicates.
		auto AppendUnique = [](TArray<FGameplayTag>& Destination, const TArray<FGameplayTag>& Source)
		{
			// Make sure the array won't allocate during the loop
			if (Destination.GetSlack() < Source.Num())
			{
				Destination.Reserve(Destination.Num() + Source.Num());
			}
			const TConstArrayView<FGameplayTag> PreModifiedDestinationView{ Destination.GetData(), Destination.Num() };

			for (const FGameplayTag& Tag : Source)
			{
				if (!Algo::Find(PreModifiedDestinationView, Tag))
				{
					Destination.Emplace(Tag);
				}
			}
		};

		// We should gather a list of tags to listen on events for
		TArray<FGameplayTag> GameplayTagsToBind;
		AppendUnique(GameplayTagsToBind, OngoingTagRequirements.IgnoreTags.GetGameplayTagArray());
		AppendUnique(GameplayTagsToBind, OngoingTagRequirements.RequireTags.GetGameplayTagArray());
		AppendUnique(GameplayTagsToBind, OngoingTagRequirements.TagQuery.GetGameplayTagArray());

		if (bAuthority || bAllowClientSideRemoval)
		{
			AppendUnique(GameplayTagsToBind, RemovalTagRequirements.IgnoreTags.GetGameplayTagArray());
			AppendUnique(GameplayTagsToBind, RemovalTagRequirements.RequireTags.GetGameplayTagArray());
			AppendUnique(GameplayTagsToBind, RemovalTagRequirements.TagQuery.GetGameplayTagArray());
		}

		// Add our tag requirements to the ASC's Callbacks map. This helps filter down the amount of callbacks we'll get due to tag changes
		// (rather than registering for the one callback whenever any tag changes).  We also need to keep track to remove those registered delegates in OnEffectRemoved.
		TArray<TTuple<FGameplayTag, FDelegateHandle>> AllBoundEvents;
		for (const FGameplayTag& Tag : GameplayTagsToBind)
		{
			FOnGameplayEffectTagCountChanged& OnTagEvent = SourceASC->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved);
			FDelegateHandle Handle = OnTagEvent.AddUObject(this, &USourceTagRequirementsGameplayEffectComponent::OnTagChanged, ActiveGEHandle);
			AllBoundEvents.Emplace(Tag, Handle);
		}

		// Now when this Effect is removed, we should remove all of our registered callbacks.
		EventSet->OnEffectRemoved.AddUObject(this, &USourceTagRequirementsGameplayEffectComponent::OnGameplayEffectRemoved, SourceASC, MoveTemp(AllBoundEvents));
	}
	else
	{
		UE_LOGF(LogGameplayEffects, Error, "USourceTagRequirementsGameplayEffectComponent::OnGameplayEffectAdded called with ActiveGE: %ls which had an invalid FActiveGameplayEffectHandle.", *ActiveGE.GetDebugString());
	}

	FGameplayTagContainer TagContainer;
	SourceASC->GetOwnedGameplayTags(TagContainer);

	return OngoingTagRequirements.RequirementsMet(TagContainer);
}

void USourceTagRequirementsGameplayEffectComponent::OnGameplayEffectRemoved(const FGameplayEffectRemovalInfo& GERemovalInfo, UAbilitySystemComponent* ASC, TArray<TTuple<FGameplayTag, FDelegateHandle>> AllBoundEvents) const
{
	for (TTuple<FGameplayTag, FDelegateHandle>& Pair : AllBoundEvents)
	{
		const bool bSuccess = ASC->UnregisterGameplayTagEvent(Pair.Value, Pair.Key, EGameplayTagEventType::NewOrRemoved);
		UE_CLOGF(!bSuccess, LogGameplayEffects, Error, "%ls tried to unregister GameplayTagEvent '%ls' on GameplayEffect '%ls' but failed.", *GetName(), *Pair.Key.ToString(), *GetNameSafe(GetOwner()));
	}
}

void USourceTagRequirementsGameplayEffectComponent::OnTagChanged(const FGameplayTag GameplayTag, int32 NewCount, FActiveGameplayEffectHandle ActiveGEHandle) const
{
	// Note: This function can remove us (RemoveActiveGameplayEffect eventually calling OnGameplayEffectRemoved),
	// but we could be in the middle of a stack of OnTagChanged callbacks, wo we could get a stale OnTagChanged.
	UAbilitySystemComponent* TargetASC = ActiveGEHandle.GetOwningAbilitySystemComponent();
	if (!TargetASC)
	{
		return;
	}

	// It's possible for this to return nullptr if it was in the process of being removed (IsPendingRemove)
	const FActiveGameplayEffect* ActiveGE = TargetASC->GetActiveGameplayEffect(ActiveGEHandle);
	if (ActiveGE)
	{
		//Need to check the source of the effect's ASC instead of the owner. If that source ASC isn't valid, just remove this effect
		//since we'd no longer be able to check tag requirements
		const UAbilitySystemComponent* SourceASC = ActiveGE->Spec.GetEffectContext().GetInstigatorAbilitySystemComponent();
		if (!SourceASC)
		{
			if (TargetASC->IsOwnerActorAuthoritative())
			{
				TargetASC->RemoveActiveGameplayEffect(ActiveGEHandle);
			}
			return;
		}
		
		FGameplayTagContainer OwnedTags;
		SourceASC->GetOwnedGameplayTags(OwnedTags);

		const bool bRemovalRequirementsMet = HaveRemovalRequirementsBeenMet(OwnedTags, TargetASC->IsOwnerActorAuthoritative());
		if (bRemovalRequirementsMet)
		{
			// This is slightly different functionality from pre-UE5.3, we're calling RemoveActiveGameplayEffect rather than InternalRemoveActiveGameplayEffect.
			// The result is we set the calculated magnitudes back to zero.  In UE5.3 this used to also run on the client (incorrect).
			TargetASC->RemoveActiveGameplayEffect(ActiveGEHandle);
		}
		else
		{
			// See if we should be inhibiting the execution
			constexpr bool bInvokeCuesIfStateChanged = true;
			const bool bOngoingRequirementsMet = OngoingTagRequirements.IsEmpty() || OngoingTagRequirements.RequirementsMet(OwnedTags);
			TargetASC->SetActiveGameplayEffectInhibit(MoveTemp(ActiveGEHandle), !bOngoingRequirementsMet, bInvokeCuesIfStateChanged);
		}
	}
}

bool USourceTagRequirementsGameplayEffectComponent::HaveRemovalRequirementsBeenMet(const FGameplayTagContainer& SourceOwnedTags, bool bNetAuthority) const
{
	using namespace UE::AbilitySystem::Private;
	const bool bAllowClientSideRemoval = EnumHasAnyFlags(static_cast<EAllowPredictiveGEFlags>(CVarAllowPredictiveGEFlagsValue), EAllowPredictiveGEFlags::AllowRemovalByTagRequirements);
	const bool bRemovalRequirementsMet = (bAllowClientSideRemoval || bNetAuthority) && !RemovalTagRequirements.IsEmpty() && RemovalTagRequirements.RequirementsMet(SourceOwnedTags);

	return bRemovalRequirementsMet;
}

#if WITH_EDITOR
/**
 * Validate incompatable configurations
 */
EDataValidationResult USourceTagRequirementsGameplayEffectComponent::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	const bool bInstantEffect = (GetOwner()->DurationPolicy == EGameplayEffectDurationType::Instant);
	if (bInstantEffect && !OngoingTagRequirements.IsEmpty())
	{
		Context.AddError(LOCTEXT("GEInstantAndOngoing", "GE is instant but has OngoingTagRequirements."));
		Result = EDataValidationResult::Invalid;
	}

	return Result;
}
#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE
