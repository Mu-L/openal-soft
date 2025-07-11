#include "config.h"

#include "fx_slots.h"

#include <array>

#include "api.h"
#include "exception.h"


namespace
{


class EaxFxSlotsException : public EaxException {
public:
    explicit EaxFxSlotsException(const std::string_view message)
        : EaxException{"EAX_FX_SLOTS", message}
    { }
}; // EaxFxSlotsException


} // namespace


void EaxFxSlots::initialize(ALCcontext& al_context)
{
    initialize_fx_slots(al_context);
}

void EaxFxSlots::uninitialize() noexcept
{
    for (auto& fx_slot : fx_slots_)
    {
        fx_slot = nullptr;
    }
}

const ALeffectslot& EaxFxSlots::get(EaxFxSlotIndex index) const
{
    if(!index.has_value())
        fail("Empty index.");
    return *fx_slots_[index.value()];
}

ALeffectslot& EaxFxSlots::get(EaxFxSlotIndex index)
{
    if(!index.has_value())
        fail("Empty index.");
    return *fx_slots_[index.value()];
}

[[noreturn]]
void EaxFxSlots::fail(const std::string_view message)
{ throw EaxFxSlotsException{message}; }

void EaxFxSlots::initialize_fx_slots(ALCcontext& al_context)
{
    auto fx_slot_index = EaxFxSlotIndexValue{};

    for(auto& fx_slot : fx_slots_)
    {
        fx_slot = eax_create_al_effect_slot(al_context);
        fx_slot->eax_initialize(al_context, fx_slot_index);
        fx_slot_index += 1;
    }
}
