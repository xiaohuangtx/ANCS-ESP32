#ifndef ANCS_DEFS_H
#define ANCS_DEFS_H

#include <cstdint>

// Apple Notification Center Service (ANCS) protocol constants
// https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/

enum AncsEventId : uint8_t {
    kAncsEventAdded = 0,
    kAncsEventModified = 1,
    kAncsEventRemoved = 2,
};

enum AncsEventFlag : uint8_t {
    kAncsFlagSilent = (1 << 0),
    kAncsFlagImportant = (1 << 1),
    kAncsFlagPreExisting = (1 << 2),
    kAncsFlagPositiveAction = (1 << 3),
    kAncsFlagNegativeAction = (1 << 4),
};

enum AncsCategoryId : uint8_t {
    kAncsCategoryOther = 0,
    kAncsCategoryIncomingCall = 1,
    kAncsCategoryMissedCall = 2,
    kAncsCategoryVoicemail = 3,
    kAncsCategorySocial = 4,
    kAncsCategorySchedule = 5,
    kAncsCategoryEmail = 6,
    kAncsCategoryNews = 7,
    kAncsCategoryHealthAndFitness = 8,
    kAncsCategoryBusinessAndFinance = 9,
    kAncsCategoryLocation = 10,
    kAncsCategoryEntertainment = 11,
};

enum AncsCommandId : uint8_t {
    kAncsCommandGetNotificationAttributes = 0,
    kAncsCommandGetAppAttributes = 1,
    kAncsCommandPerformNotificationAction = 2,
};

enum AncsNotificationAttributeId : uint8_t {
    kAncsAttrAppIdentifier = 0,
    kAncsAttrTitle = 1,           // Needs max length parameter
    kAncsAttrSubtitle = 2,        // Needs max length parameter
    kAncsAttrMessage = 3,         // Needs max length parameter
    kAncsAttrMessageSize = 4,
    kAncsAttrDate = 5,            // "yyyyMMdd'T'HHmmSS"
    kAncsAttrPositiveActionLabel = 6,
    kAncsAttrNegativeActionLabel = 7,
};

enum AncsAppAttributeId : uint8_t {
    kAncsAppAttrDisplayName = 0,
};

enum AncsActionId : uint8_t {
    kAncsActionPositive = 0,
    kAncsActionNegative = 1,
};

// ATT error codes returned by the Control Point
enum AncsErrorCode : uint8_t {
    kAncsErrorUnknownCommand = 0xA0,
    kAncsErrorInvalidCommand = 0xA1,
    kAncsErrorInvalidParameter = 0xA2,
    kAncsErrorActionFailed = 0xA3,
};

const char* AncsEventName(uint8_t event_id);
const char* AncsCategoryName(uint8_t category_id);
const char* AncsErrorName(uint8_t error_code);

#endif // ANCS_DEFS_H
