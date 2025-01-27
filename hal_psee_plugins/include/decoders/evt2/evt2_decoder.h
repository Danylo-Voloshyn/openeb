/**********************************************************************************************************************
 * Copyright (c) Prophesee S.A.                                                                                       *
 *                                                                                                                    *
 * Licensed under the Apache License, Version 2.0 (the "License");                                                    *
 * you may not use this file except in compliance with the License.                                                   *
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0                                 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed   *
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.                      *
 * See the License for the specific language governing permissions and limitations under the License.                 *
 **********************************************************************************************************************/

#ifndef METAVISION_HAL_EVT2_DECODER_H
#define METAVISION_HAL_EVT2_DECODER_H

#include "metavision/hal/facilities/i_decoder.h"
#include "metavision/sdk/base/events/event_cd.h"
#include "metavision/sdk/base/events/event_ext_trigger.h"
#include "metavision/hal/facilities/i_event_decoder.h"
#include "decoders/base/event_base.h"
#include "decoders/evt2/evt2_event_types.h"

namespace Metavision {

class EVT2Decoder : public I_Decoder {
public:
    using RawEvent        = EventBase::RawEvent;
    using EventTypesEnum  = EVT2EventTypes;
    using Event_Word_Type = uint32_t;

    static constexpr std::uint8_t NumBitsInTimestampLSB = EVT2EventsTimeStampBits;
    static constexpr timestamp MaxTimestamp             = timestamp((1 << 28) - 1) << NumBitsInTimestampLSB;
    static constexpr timestamp LoopThreshold            = 10000;
    static constexpr timestamp TimeLoop                 = MaxTimestamp + (1 << NumBitsInTimestampLSB);

    EVT2Decoder(
        bool time_shifting_enabled,
        const std::shared_ptr<I_EventDecoder<EventCD>> &event_cd_decoder = std::shared_ptr<I_EventDecoder<EventCD>>(),
        const std::shared_ptr<I_EventDecoder<EventExtTrigger>> &event_ext_trigger_decoder =
            std::shared_ptr<I_EventDecoder<EventExtTrigger>>()) :
        I_Decoder(time_shifting_enabled, event_cd_decoder, event_ext_trigger_decoder) {}

    virtual bool get_timestamp_shift(timestamp &ts_shift) const override {
        ts_shift = shift_th_;
        return base_time_set_;
    }

    virtual timestamp get_last_timestamp() const override {
        return last_timestamp_;
    }

    uint8_t get_raw_event_size_bytes() const override {
        return sizeof(RawEvent);
    }

private:
    virtual void decode_impl(RawData *cur_raw_data, RawData *raw_data_end) override {
        RawEvent *cur_raw_ev = reinterpret_cast<RawEvent *>(cur_raw_data);
        RawEvent *raw_ev_end = reinterpret_cast<RawEvent *>(raw_data_end);

        if (!base_time_set_) {
            for (; cur_raw_ev != raw_ev_end; cur_raw_ev++) {
                EventBase::RawEvent *ev = reinterpret_cast<EventBase::RawEvent *>(cur_raw_ev);
                if (ev->type == static_cast<EventTypesUnderlying_t>(EventTypesEnum::EVT_TIME_HIGH)) {
                    uint64_t t = ev->trail;
                    t <<= NumBitsInTimestampLSB;
                    base_time_     = t;
                    shift_th_      = is_time_shifting_enabled() ? t : 0;
                    full_shift_    = -shift_th_;
                    base_time_set_ = true;
                    break;
                }
            }
        }

        if (!buffer_has_time_loop(cur_raw_ev, raw_ev_end, base_time_, full_shift_)) {
            // In the general case: if no time shift is to be applied and there is no time loop yet, do not apply
            // any shifting on the new timer high decoded.
            if (full_shift_ == 0) {
                decode_events_buffer<false, false>(cur_raw_ev, raw_ev_end);
            } else {
                decode_events_buffer<false, true>(cur_raw_ev, raw_ev_end);
            }
        } else {
            decode_events_buffer<true, true>(cur_raw_ev, raw_ev_end);
        }
    }

    template<bool UPDATE_LOOP, bool APPLY_TIMESHIFT>
    void decode_events_buffer(RawEvent *&cur_raw_ev, RawEvent *const raw_ev_end) {
        auto &cd_forwarder      = cd_event_forwarder();
        auto &trigger_forwarder = trigger_event_forwarder();
        for (; cur_raw_ev != raw_ev_end; ++cur_raw_ev) {
            EventBase::RawEvent *ev = reinterpret_cast<EventBase::RawEvent *>(cur_raw_ev);
            const unsigned int type = ev->type;
            if (type == static_cast<EventTypesUnderlying_t>(EventTypesEnum::EVT_TIME_HIGH)) {
                timestamp new_th = timestamp(ev->trail) << NumBitsInTimestampLSB;
                if (UPDATE_LOOP) {
                    new_th += full_shift_;
                    if (has_time_loop(new_th, base_time_)) {
                        full_shift_ += TimeLoop;
                        new_th += TimeLoop;
                    }
                    base_time_ = new_th;
                } else {
                    base_time_ = APPLY_TIMESHIFT ? new_th + full_shift_ : new_th;
                }
            } else if (type == static_cast<EventTypesUnderlying_t>(EventTypesEnum::LEFT_TD_LOW) ||
                       type == static_cast<EventTypesUnderlying_t>(EventTypesEnum::LEFT_TD_HIGH)) { // CD
                EVT2Event2D *ev_td = reinterpret_cast<EVT2Event2D *>(ev);
                last_timestamp_    = base_time_ + ev_td->timestamp;
                cd_forwarder.forward(static_cast<unsigned short>(ev_td->x), static_cast<unsigned short>(ev_td->y),
                                     ev_td->type & 1, last_timestamp_);
            } else if (type == static_cast<EventTypesUnderlying_t>(EventTypesEnum::EXT_TRIGGER)) {
                EVT2EventExtTrigger *ev_ext_raw = reinterpret_cast<EVT2EventExtTrigger *>(ev);
                last_timestamp_                 = base_time_ + ev_ext_raw->timestamp;
                trigger_forwarder.forward(static_cast<short>(ev_ext_raw->value), last_timestamp_,
                                          static_cast<short>(ev_ext_raw->id));
            }
        }
    }

    static bool buffer_has_time_loop(RawEvent *const cur_raw_ev, RawEvent *raw_ev_end, const timestamp base_time_us,
                                     const timestamp timeshift_us) {
        for (; raw_ev_end != cur_raw_ev;) {
            --raw_ev_end; // If cur_ev_end == cur_ev, we don't enter so cur_ev_end is always valid
            if (raw_ev_end->type == static_cast<EventTypesUnderlying_t>(EventTypesEnum::EVT_TIME_HIGH)) {
                const timestamp timer_high = (timestamp(raw_ev_end->trail) << NumBitsInTimestampLSB) + timeshift_us;
                return has_time_loop(timer_high, base_time_us);
            }
        }
        return false;
    }

    static bool has_time_loop(const timestamp current_time_us, const timestamp base_time_us) {
        return (current_time_us < base_time_us) && ((base_time_us - current_time_us) >= (MaxTimestamp - LoopThreshold));
    }

    bool base_time_set_ = false;

    timestamp base_time_;          // base time to add non timer high events' ts to
    timestamp shift_th_{0};        // first th decoded
    timestamp last_timestamp_{-1}; // ts of the last event
    timestamp full_shift_{
        0}; // includes loop and shift_th in one single variable. Must be signed typed as shift can be negative.
};

} // namespace Metavision

#endif // METAVISION_HAL_EVT2_DECODER_H
