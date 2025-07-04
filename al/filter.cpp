/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "filter.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <span>
#include <unordered_map>
#include <vector>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/efx.h"

#include "alc/context.h"
#include "alc/device.h"
#include "almalloc.h"
#include "alnumeric.h"
#include "core/except.h"
#include "core/logging.h"
#include "direct_defs.h"
#include "gsl/gsl"
#include "intrusive_ptr.h"
#include "opthelpers.h"

using uint = unsigned int;


namespace {

using SubListAllocator = al::allocator<std::array<ALfilter,64>>;


void InitFilterParams(gsl::not_null<ALfilter*> filter, ALenum type)
{
    if(type == AL_FILTER_LOWPASS)
    {
        filter->Gain = AL_LOWPASS_DEFAULT_GAIN;
        filter->GainHF = AL_LOWPASS_DEFAULT_GAINHF;
        filter->HFReference = LowPassFreqRef;
        filter->GainLF = 1.0f;
        filter->LFReference = HighPassFreqRef;
        filter->mTypeVariant.emplace<LowpassFilterTable>();
    }
    else if(type == AL_FILTER_HIGHPASS)
    {
        filter->Gain = AL_HIGHPASS_DEFAULT_GAIN;
        filter->GainHF = 1.0f;
        filter->HFReference = LowPassFreqRef;
        filter->GainLF = AL_HIGHPASS_DEFAULT_GAINLF;
        filter->LFReference = HighPassFreqRef;
        filter->mTypeVariant.emplace<HighpassFilterTable>();
    }
    else if(type == AL_FILTER_BANDPASS)
    {
        filter->Gain = AL_BANDPASS_DEFAULT_GAIN;
        filter->GainHF = AL_BANDPASS_DEFAULT_GAINHF;
        filter->HFReference = LowPassFreqRef;
        filter->GainLF = AL_BANDPASS_DEFAULT_GAINLF;
        filter->LFReference = HighPassFreqRef;
        filter->mTypeVariant.emplace<BandpassFilterTable>();
    }
    else
    {
        filter->Gain = 1.0f;
        filter->GainHF = 1.0f;
        filter->HFReference = LowPassFreqRef;
        filter->GainLF = 1.0f;
        filter->LFReference = HighPassFreqRef;
        filter->mTypeVariant.emplace<NullFilterTable>();
    }
    filter->type = type;
}

[[nodiscard]]
auto EnsureFilters(al::Device *device, size_t needed) noexcept -> bool
try {
    auto count = std::accumulate(device->FilterList.cbegin(), device->FilterList.cend(), 0_uz,
        [](size_t cur, const FilterSubList &sublist) noexcept -> size_t
        { return cur + gsl::narrow_cast<uint>(std::popcount(sublist.FreeMask)); });

    while(needed > count)
    {
        if(device->FilterList.size() >= 1<<25) [[unlikely]]
            return false;

        auto sublist = FilterSubList{};
        sublist.FreeMask = ~0_u64;
        sublist.Filters = SubListAllocator{}.allocate(1);
        device->FilterList.emplace_back(std::move(sublist));
        count += std::tuple_size_v<SubListAllocator::value_type>;
    }
    return true;
}
catch(...) {
    return false;
}


[[nodiscard]]
auto AllocFilter(al::Device *device) noexcept -> gsl::not_null<ALfilter*>
{
    auto sublist = std::ranges::find_if(device->FilterList, &FilterSubList::FreeMask);
    auto lidx = gsl::narrow_cast<uint>(std::distance(device->FilterList.begin(), sublist));
    auto slidx = gsl::narrow_cast<uint>(std::countr_zero(sublist->FreeMask));
    ASSUME(slidx < 64);

    auto *filter = std::construct_at(std::to_address(std::next(sublist->Filters->begin(), slidx)));
    InitFilterParams(filter, AL_FILTER_NULL);

    /* Add 1 to avoid filter ID 0. */
    filter->id = ((lidx<<6) | slidx) + 1;

    sublist->FreeMask &= ~(1_u64 << slidx);

    return filter;
}

void FreeFilter(al::Device *device, gsl::not_null<ALfilter*> filter)
{
    device->mFilterNames.erase(filter->id);

    const auto id = filter->id - 1;
    const auto lidx = id >> 6;
    const auto slidx = id & 0x3f;

    std::destroy_at(filter.get());

    device->FilterList[lidx].FreeMask |= 1_u64 << slidx;
}

[[nodiscard]]
inline auto LookupFilter(std::nothrow_t, al::Device *device, ALuint id) noexcept -> ALfilter*
{
    const auto lidx = (id-1) >> 6;
    const auto slidx = (id-1) & 0x3f;

    if(lidx >= device->FilterList.size()) [[unlikely]]
        return nullptr;
    auto &sublist = device->FilterList[lidx];
    if(sublist.FreeMask & (1_u64 << slidx)) [[unlikely]]
        return nullptr;
    return std::to_address(std::next(sublist.Filters->begin(), slidx));
}

[[nodiscard]]
auto LookupFilter(gsl::not_null<ALCcontext*> context, ALuint id) -> gsl::not_null<ALfilter*>
{
    if(auto *filter = LookupFilter(std::nothrow, context->mALDevice.get(), id)) [[likely]]
        return filter;
    context->throw_error(AL_INVALID_NAME, "Invalid filter ID {}", id);
}

} // namespace

/* Null filter parameter handlers */
template<>
void FilterTable<NullFilterTable>::setParami(ALCcontext *context, ALfilter*, ALenum param, int)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<NullFilterTable>::setParamiv(ALCcontext *context, ALfilter*, ALenum param, const int*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<NullFilterTable>::setParamf(ALCcontext *context, ALfilter*, ALenum param, float)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<NullFilterTable>::setParamfv(ALCcontext *context, ALfilter*, ALenum param, const float*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<NullFilterTable>::getParami(ALCcontext *context, const ALfilter*, ALenum param, int*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<NullFilterTable>::getParamiv(ALCcontext *context, const ALfilter*, ALenum param, int*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<NullFilterTable>::getParamf(ALCcontext *context, const ALfilter*, ALenum param, float*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<NullFilterTable>::getParamfv(ALCcontext *context, const ALfilter*, ALenum param, float*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid null filter property {:#04x}", as_unsigned(param)); }

/* Lowpass parameter handlers */
template<>
void FilterTable<LowpassFilterTable>::setParami(ALCcontext *context, ALfilter*, ALenum param, int)
{ context->throw_error(AL_INVALID_ENUM, "Invalid low-pass integer property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<LowpassFilterTable>::setParamiv(ALCcontext *context, ALfilter *filter, ALenum param, const int *values)
{ setParami(context, filter, param, *values); }
template<>
void FilterTable<LowpassFilterTable>::setParamf(ALCcontext *context, ALfilter *filter, ALenum param, float val)
{
    switch(param)
    {
    case AL_LOWPASS_GAIN:
        if(!(val >= AL_LOWPASS_MIN_GAIN && val <= AL_LOWPASS_MAX_GAIN))
            context->throw_error(AL_INVALID_VALUE, "Low-pass gain {} out of range", val);
        filter->Gain = val;
        return;

    case AL_LOWPASS_GAINHF:
        if(!(val >= AL_LOWPASS_MIN_GAINHF && val <= AL_LOWPASS_MAX_GAINHF))
            context->throw_error(AL_INVALID_VALUE, "Low-pass gainhf {} out of range", val);
        filter->GainHF = val;
        return;
    }
    context->throw_error(AL_INVALID_ENUM, "Invalid low-pass float property {:#04x}",
        as_unsigned(param));
}
template<>
void FilterTable<LowpassFilterTable>::setParamfv(ALCcontext *context, ALfilter *filter, ALenum param, const float *vals)
{ setParamf(context, filter, param, *vals); }
template<>
void FilterTable<LowpassFilterTable>::getParami(ALCcontext *context, const ALfilter*, ALenum param, int*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid low-pass integer property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<LowpassFilterTable>::getParamiv(ALCcontext *context, const ALfilter *filter, ALenum param, int *values)
{ getParami(context, filter, param, values); }
template<>
void FilterTable<LowpassFilterTable>::getParamf(ALCcontext *context, const ALfilter *filter, ALenum param, float *val)
{
    switch(param)
    {
    case AL_LOWPASS_GAIN: *val = filter->Gain; return;
    case AL_LOWPASS_GAINHF: *val = filter->GainHF; return;
    }
    context->throw_error(AL_INVALID_ENUM, "Invalid low-pass float property {:#04x}",
        as_unsigned(param));
}
template<>
void FilterTable<LowpassFilterTable>::getParamfv(ALCcontext *context, const ALfilter *filter, ALenum param, float *vals)
{ getParamf(context, filter, param, vals); }

/* Highpass parameter handlers */
template<>
void FilterTable<HighpassFilterTable>::setParami(ALCcontext *context, ALfilter*, ALenum param, int)
{ context->throw_error(AL_INVALID_ENUM, "Invalid high-pass integer property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<HighpassFilterTable>::setParamiv(ALCcontext *context, ALfilter *filter, ALenum param, const int *values)
{ setParami(context, filter, param, *values); }
template<>
void FilterTable<HighpassFilterTable>::setParamf(ALCcontext *context, ALfilter *filter, ALenum param, float val)
{
    switch(param)
    {
    case AL_HIGHPASS_GAIN:
        if(!(val >= AL_HIGHPASS_MIN_GAIN && val <= AL_HIGHPASS_MAX_GAIN))
            context->throw_error(AL_INVALID_VALUE, "High-pass gain {} out of range", val);
        filter->Gain = val;
        return;

    case AL_HIGHPASS_GAINLF:
        if(!(val >= AL_HIGHPASS_MIN_GAINLF && val <= AL_HIGHPASS_MAX_GAINLF))
            context->throw_error(AL_INVALID_VALUE, "High-pass gainlf {} out of range", val);
        filter->GainLF = val;
        return;
    }
    context->throw_error(AL_INVALID_ENUM, "Invalid high-pass float property {:#04x}",
        as_unsigned(param));
}
template<>
void FilterTable<HighpassFilterTable>::setParamfv(ALCcontext *context, ALfilter *filter, ALenum param, const float *vals)
{ setParamf(context, filter, param, *vals); }
template<>
void FilterTable<HighpassFilterTable>::getParami(ALCcontext *context, const ALfilter*, ALenum param, int*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid high-pass integer property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<HighpassFilterTable>::getParamiv(ALCcontext *context, const ALfilter *filter, ALenum param, int *values)
{ getParami(context, filter, param, values); }
template<>
void FilterTable<HighpassFilterTable>::getParamf(ALCcontext *context, const ALfilter *filter, ALenum param, float *val)
{
    switch(param)
    {
    case AL_HIGHPASS_GAIN: *val = filter->Gain; return;
    case AL_HIGHPASS_GAINLF: *val = filter->GainLF; return;
    }
    context->throw_error(AL_INVALID_ENUM, "Invalid high-pass float property {:#04x}",
        as_unsigned(param));
}
template<>
void FilterTable<HighpassFilterTable>::getParamfv(ALCcontext *context, const ALfilter *filter, ALenum param, float *vals)
{ getParamf(context, filter, param, vals); }

/* Bandpass parameter handlers */
template<>
void FilterTable<BandpassFilterTable>::setParami(ALCcontext *context, ALfilter*, ALenum param, int)
{ context->throw_error(AL_INVALID_ENUM, "Invalid band-pass integer property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<BandpassFilterTable>::setParamiv(ALCcontext *context, ALfilter *filter, ALenum param, const int *values)
{ setParami(context, filter, param, *values); }
template<>
void FilterTable<BandpassFilterTable>::setParamf(ALCcontext *context, ALfilter *filter, ALenum param, float val)
{
    switch(param)
    {
    case AL_BANDPASS_GAIN:
        if(!(val >= AL_BANDPASS_MIN_GAIN && val <= AL_BANDPASS_MAX_GAIN))
            context->throw_error(AL_INVALID_VALUE, "Band-pass gain {} out of range", val);
        filter->Gain = val;
        return;

    case AL_BANDPASS_GAINHF:
        if(!(val >= AL_BANDPASS_MIN_GAINHF && val <= AL_BANDPASS_MAX_GAINHF))
            context->throw_error(AL_INVALID_VALUE, "Band-pass gainhf {} out of range", val);
        filter->GainHF = val;
        return;

    case AL_BANDPASS_GAINLF:
        if(!(val >= AL_BANDPASS_MIN_GAINLF && val <= AL_BANDPASS_MAX_GAINLF))
            context->throw_error(AL_INVALID_VALUE, "Band-pass gainlf {} out of range", val);
        filter->GainLF = val;
        return;
    }
    context->throw_error(AL_INVALID_ENUM, "Invalid band-pass float property {:#04x}",
        as_unsigned(param));
}
template<>
void FilterTable<BandpassFilterTable>::setParamfv(ALCcontext *context, ALfilter *filter, ALenum param, const float *vals)
{ setParamf(context, filter, param, *vals); }
template<>
void FilterTable<BandpassFilterTable>::getParami(ALCcontext *context, const ALfilter*, ALenum param, int*)
{ context->throw_error(AL_INVALID_ENUM, "Invalid band-pass integer property {:#04x}", as_unsigned(param)); }
template<>
void FilterTable<BandpassFilterTable>::getParamiv(ALCcontext *context, const ALfilter *filter, ALenum param, int *values)
{ getParami(context, filter, param, values); }
template<>
void FilterTable<BandpassFilterTable>::getParamf(ALCcontext *context, const ALfilter *filter, ALenum param, float *val)
{
    switch(param)
    {
    case AL_BANDPASS_GAIN: *val = filter->Gain; return;
    case AL_BANDPASS_GAINHF: *val = filter->GainHF; return;
    case AL_BANDPASS_GAINLF: *val = filter->GainLF; return;
    }
    context->throw_error(AL_INVALID_ENUM, "Invalid band-pass float property {:#04x}",
        as_unsigned(param));
}
template<>
void FilterTable<BandpassFilterTable>::getParamfv(ALCcontext *context, const ALfilter *filter, ALenum param, float *vals)
{ getParamf(context, filter, param, vals); }


AL_API DECL_FUNC2(void, alGenFilters, ALsizei,n, ALuint*,filters)
FORCE_ALIGN void AL_APIENTRY alGenFiltersDirect(ALCcontext *context, ALsizei n, ALuint *filters) noexcept
try {
    if(n < 0)
        context->throw_error(AL_INVALID_VALUE, "Generating {} filters", n);
    if(n <= 0) [[unlikely]] return;

    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    const auto fids = std::span{filters, gsl::narrow_cast<uint>(n)};
    if(!EnsureFilters(device, fids.size()))
        context->throw_error(AL_OUT_OF_MEMORY, "Failed to allocate {} filter{}", n,
            (n==1) ? "" : "s");

    std::ranges::generate(fids, [device]{ return AllocFilter(device)->id; });
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC2(void, alDeleteFilters, ALsizei,n, const ALuint*,filters)
FORCE_ALIGN void AL_APIENTRY alDeleteFiltersDirect(ALCcontext *context, ALsizei n,
    const ALuint *filters) noexcept
try {
    if(n < 0)
        context->throw_error(AL_INVALID_VALUE, "Deleting {} filters", n);
    if(n <= 0) [[unlikely]] return;

    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    /* First try to find any filters that are invalid. */
    const auto fids = std::span{filters, gsl::narrow_cast<uint>(n)};
    std::ranges::for_each(fids, [context](const ALuint fid)
    { if(fid != 0) std::ignore = LookupFilter(context, fid); });

    /* All good. Delete non-0 filter IDs. */
    std::ranges::for_each(fids, [device](const ALuint fid)
    {
        if(auto *filter = LookupFilter(std::nothrow, device, fid))
            FreeFilter(device, filter);
    });
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC1(ALboolean, alIsFilter, ALuint,filter)
FORCE_ALIGN ALboolean AL_APIENTRY alIsFilterDirect(ALCcontext *context, ALuint filter) noexcept
{
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};
    if(filter == 0 || LookupFilter(std::nothrow, device, filter) != nullptr)
        return AL_TRUE;
    return AL_FALSE;
}


AL_API DECL_FUNC3(void, alFilteri, ALuint,filter, ALenum,param, ALint,value)
FORCE_ALIGN void AL_APIENTRY alFilteriDirect(ALCcontext *context, ALuint filter, ALenum param,
    ALint value) noexcept
try {
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);
    switch(param)
    {
    case AL_FILTER_TYPE:
        if(!(value == AL_FILTER_NULL || value == AL_FILTER_LOWPASS
            || value == AL_FILTER_HIGHPASS || value == AL_FILTER_BANDPASS))
            context->throw_error(AL_INVALID_VALUE, "Invalid filter type {:#04x}",
                as_unsigned(value));
        InitFilterParams(alfilt, value);
        return;
    }

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,value](auto&& thunk)
    { thunk.setParami(context, alfilt, param, value); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC3(void, alFilteriv, ALuint,filter, ALenum,param, const ALint*,values)
FORCE_ALIGN void AL_APIENTRY alFilterivDirect(ALCcontext *context, ALuint filter, ALenum param,
    const ALint *values) noexcept
try {
    switch(param)
    {
    case AL_FILTER_TYPE:
        alFilteriDirect(context, filter, param, *values);
        return;
    }

    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,values](auto&& thunk)
    { thunk.setParamiv(context, alfilt, param, values); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC3(void, alFilterf, ALuint,filter, ALenum,param, ALfloat,value)
FORCE_ALIGN void AL_APIENTRY alFilterfDirect(ALCcontext *context, ALuint filter, ALenum param,
    ALfloat value) noexcept
try {
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,value](auto&& thunk)
    { thunk.setParamf(context, alfilt, param, value); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC3(void, alFilterfv, ALuint,filter, ALenum,param, const ALfloat*,values)
FORCE_ALIGN void AL_APIENTRY alFilterfvDirect(ALCcontext *context, ALuint filter, ALenum param,
    const ALfloat *values) noexcept
try {
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,values](auto&& thunk)
    { thunk.setParamfv(context, alfilt, param, values); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC3(void, alGetFilteri, ALuint,filter, ALenum,param, ALint*,value)
FORCE_ALIGN void AL_APIENTRY alGetFilteriDirect(ALCcontext *context, ALuint filter, ALenum param,
    ALint *value) noexcept
try {
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);

    switch(param)
    {
    case AL_FILTER_TYPE: *value = alfilt->type; return;
    }

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,value](auto&& thunk)
    { thunk.getParami(context, alfilt, param, value); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC3(void, alGetFilteriv, ALuint,filter, ALenum,param, ALint*,values)
FORCE_ALIGN void AL_APIENTRY alGetFilterivDirect(ALCcontext *context, ALuint filter, ALenum param,
    ALint *values) noexcept
try {
    switch(param)
    {
    case AL_FILTER_TYPE:
        alGetFilteriDirect(context, filter, param, values);
        return;
    }

    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,values](auto&& thunk)
    { thunk.getParamiv(context, alfilt, param, values); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC3(void, alGetFilterf, ALuint,filter, ALenum,param, ALfloat*,value)
FORCE_ALIGN void AL_APIENTRY alGetFilterfDirect(ALCcontext *context, ALuint filter, ALenum param,
    ALfloat *value) noexcept
try {
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,value](auto&& thunk)
    { thunk.getParamf(context, alfilt, param, value); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}

AL_API DECL_FUNC3(void, alGetFilterfv, ALuint,filter, ALenum,param, ALfloat*,values)
FORCE_ALIGN void AL_APIENTRY alGetFilterfvDirect(ALCcontext *context, ALuint filter, ALenum param,
    ALfloat *values) noexcept
try {
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    auto const alfilt = LookupFilter(context, filter);

    /* Call the appropriate handler */
    std::visit([context,alfilt,param,values](auto&& thunk)
    { thunk.getParamfv(context, alfilt, param, values); }, alfilt->mTypeVariant);
}
catch(al::base_exception&) {
}
catch(std::exception &e) {
    ERR("Caught exception: {}", e.what());
}


void ALfilter::SetName(gsl::not_null<ALCcontext*> context, ALuint id, std::string_view name)
{
    auto *device = context->mALDevice.get();
    auto filterlock = std::lock_guard{device->FilterLock};

    std::ignore = LookupFilter(context, id);

    device->mFilterNames.insert_or_assign(id, name);
}


FilterSubList::~FilterSubList()
{
    if(!Filters)
        return;

    auto usemask = ~FreeMask;
    while(usemask)
    {
        auto const idx = std::countr_zero(usemask);
        std::destroy_at(std::to_address(std::next(Filters->begin(), idx)));
        usemask &= ~(1_u64 << idx);
    }
    FreeMask = ~usemask;
    SubListAllocator{}.deallocate(Filters, 1);
    Filters = nullptr;
}
