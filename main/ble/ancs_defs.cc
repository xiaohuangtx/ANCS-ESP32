#include "ancs_defs.h"

const char* AncsEventName(uint8_t event_id) {
    switch (event_id) {
        case kAncsEventAdded: return "added";
        case kAncsEventModified: return "modified";
        case kAncsEventRemoved: return "removed";
        default: return "unknown";
    }
}

const char* AncsCategoryName(uint8_t category_id) {
    switch (category_id) {
        case kAncsCategoryOther: return "Other";
        case kAncsCategoryIncomingCall: return "IncomingCall";
        case kAncsCategoryMissedCall: return "MissedCall";
        case kAncsCategoryVoicemail: return "Voicemail";
        case kAncsCategorySocial: return "Social";
        case kAncsCategorySchedule: return "Schedule";
        case kAncsCategoryEmail: return "Email";
        case kAncsCategoryNews: return "News";
        case kAncsCategoryHealthAndFitness: return "HealthAndFitness";
        case kAncsCategoryBusinessAndFinance: return "BusinessAndFinance";
        case kAncsCategoryLocation: return "Location";
        case kAncsCategoryEntertainment: return "Entertainment";
        default: return "Unknown";
    }
}

const char* AncsErrorName(uint8_t error_code) {
    switch (error_code) {
        case kAncsErrorUnknownCommand: return "UnknownCommand";
        case kAncsErrorInvalidCommand: return "InvalidCommand";
        case kAncsErrorInvalidParameter: return "InvalidParameter";
        case kAncsErrorActionFailed: return "ActionFailed";
        default: return "ATT error";
    }
}
