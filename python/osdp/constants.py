#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import osdp_sys

class LibFlag:
    EnforceSecure = osdp_sys.FLAG_ENFORCE_SECURE
    InstallMode = osdp_sys.FLAG_INSTALL_MODE
    IgnoreUnsolicited = osdp_sys.FLAG_IGN_UNSOLICITED
    EnableNotification = osdp_sys.FLAG_ENABLE_NOTIFICATION
    CapturePackets = osdp_sys.FLAG_CAPTURE_PACKETS
    AllowEmptyEncryptedDataBlock = osdp_sys.FLAG_ALLOW_EMPTY_ENCRYPTED_DATA_BLOCK

class LogLevel:
    Emergency = osdp_sys.LOG_EMERG
    Alert = osdp_sys.LOG_ALERT
    Critical = osdp_sys.LOG_CRIT
    Error = osdp_sys.LOG_ERROR
    Warning = osdp_sys.LOG_WARNING
    Notice = osdp_sys.LOG_NOTICE
    Info = osdp_sys.LOG_INFO
    Debug = osdp_sys.LOG_DEBUG

class StatusReportType:
    Local = osdp_sys.STATUS_REPORT_LOCAL
    Input = osdp_sys.STATUS_REPORT_INPUT
    Output = osdp_sys.STATUS_REPORT_OUTPUT
    Remote = osdp_sys.STATUS_REPORT_REMOTE

class Command:
    Output = osdp_sys.CMD_OUTPUT
    Buzzer = osdp_sys.CMD_BUZZER
    LED = osdp_sys.CMD_LED
    Comset = osdp_sys.CMD_COMSET
    ComsetDone = osdp_sys.CMD_COMSET_DONE
    Text = osdp_sys.CMD_TEXT
    Manufacturer = osdp_sys.CMD_MFG
    Keyset = osdp_sys.CMD_KEYSET
    FileTransfer = osdp_sys.CMD_FILE_TX
    Status = osdp_sys.CMD_STATUS
    Notification = osdp_sys.CMD_NOTIFICATION
    BioRead = osdp_sys.CMD_BIOREAD
    BioMatch = osdp_sys.CMD_BIOMATCH

class CommandLEDColor:
    Black = osdp_sys.LED_COLOR_NONE
    Red = osdp_sys.LED_COLOR_RED
    Green = osdp_sys.LED_COLOR_GREEN
    Amber = osdp_sys.LED_COLOR_AMBER
    Blue = osdp_sys.LED_COLOR_BLUE
    Magenta = osdp_sys.LED_COLOR_MAGENTA
    Cyan = osdp_sys.LED_COLOR_CYAN
    White = osdp_sys.LED_COLOR_WHITE

class CommandFileTxFlags:
    Cancel = osdp_sys.CMD_FILE_TX_FLAG_CANCEL

class Notification:
    Command = osdp_sys.NOTIFICATION_COMMAND
    SecureChannelStatus = osdp_sys.NOTIFICATION_SC_STATUS
    PeripheralDeviceStatus = osdp_sys.NOTIFICATION_PD_STATUS
    FileTransferDone = osdp_sys.NOTIFICATION_FILE_TX_DONE

class FileTxOutcome:
    Ok = osdp_sys.FILE_TX_OUTCOME_OK
    OkRebooting = osdp_sys.FILE_TX_OUTCOME_OK_REBOOTING
    Aborted = osdp_sys.FILE_TX_OUTCOME_ABORTED
    Unrecognized = osdp_sys.FILE_TX_OUTCOME_UNRECOGNIZED
    Invalid = osdp_sys.FILE_TX_OUTCOME_INVALID

class CompletionStatus:
    Ok = getattr(osdp_sys, "COMPLETION_OK", 0)
    Failed = getattr(osdp_sys, "COMPLETION_FAILED", 1)
    Flushed = getattr(osdp_sys, "COMPLETION_FLUSHED", 2)
    Aborted = getattr(osdp_sys, "COMPLETION_ABORTED", 3)

class Event:
    CardRead = osdp_sys.EVENT_CARDREAD
    KeyPress = osdp_sys.EVENT_KEYPRESS
    ManufacturerReply = osdp_sys.EVENT_MFGREP
    ManufacturerStatus = osdp_sys.EVENT_MFGSTATR
    ManufacturerError = osdp_sys.EVENT_MFGERRR
    Status = osdp_sys.EVENT_STATUS
    Notification = osdp_sys.EVENT_NOTIFICATION
    BioRead = osdp_sys.EVENT_BIOREADR
    BioMatch = osdp_sys.EVENT_BIOMATCHR

class BioType:
    NotSpecified = osdp_sys.BIO_TYPE_NOT_SPECIFIED
    RightThumbPrint = osdp_sys.BIO_TYPE_RIGHT_THUMB_PRINT
    RightIndexFingerPrint = osdp_sys.BIO_TYPE_RIGHT_INDEX_FINGER_PRINT
    RightMiddleFingerPrint = osdp_sys.BIO_TYPE_RIGHT_MIDDLE_FINGER_PRINT
    RightRingFingerPrint = osdp_sys.BIO_TYPE_RIGHT_RING_FINGER_PRINT
    RightLittleFingerPrint = osdp_sys.BIO_TYPE_RIGHT_LITTLE_FINGER_PRINT
    LeftThumbPrint = osdp_sys.BIO_TYPE_LEFT_THUMB_PRINT
    LeftIndexFingerPrint = osdp_sys.BIO_TYPE_LEFT_INDEX_FINGER_PRINT
    LeftMiddleFingerPrint = osdp_sys.BIO_TYPE_LEFT_MIDDLE_FINGER_PRINT
    LeftRingFingerPrint = osdp_sys.BIO_TYPE_LEFT_RING_FINGER_PRINT
    LeftLittleFingerPrint = osdp_sys.BIO_TYPE_LEFT_LITTLE_FINGER_PRINT
    RightIrisScan = osdp_sys.BIO_TYPE_RIGHT_IRIS_SCAN
    RightRetinaScan = osdp_sys.BIO_TYPE_RIGHT_RETINA_SCAN
    LeftIrisScan = osdp_sys.BIO_TYPE_LEFT_IRIS_SCAN
    LeftRetinaScan = osdp_sys.BIO_TYPE_LEFT_RETINA_SCAN
    FullFaceImage = osdp_sys.BIO_TYPE_FULL_FACE_IMAGE
    RightHandGeometry = osdp_sys.BIO_TYPE_RIGHT_HAND_GEOMETRY
    LeftHandGeometry = osdp_sys.BIO_TYPE_LEFT_HAND_GEOMETRY

class BioFormat:
    NotSpecified = osdp_sys.BIO_FMT_NOT_SPECIFIED
    RawPGM = osdp_sys.BIO_FMT_RAW_PGM
    AnsiIncits378 = osdp_sys.BIO_FMT_ANSI_INCITS_378

class BioStatus:
    Success = osdp_sys.BIO_STATUS_SUCCESS
    Timeout = osdp_sys.BIO_STATUS_TIMEOUT
    UnknownError = osdp_sys.BIO_STATUS_UNKNOWN_ERROR

class Nak:
    """NAK codes a PD command handler may return, negated.

    e.g. `return -Nak.BioType, None` to reject an unsupported biometric type.
    """
    BioType = osdp_sys.PD_NAK_BIO_TYPE
    BioFormat = osdp_sys.PD_NAK_BIO_FMT
    Record = osdp_sys.PD_NAK_RECORD

class CardFormat:
    Unspecified = osdp_sys.CARD_FMT_RAW_UNSPECIFIED
    Wiegand = osdp_sys.CARD_FMT_RAW_WIEGAND
    ASCII = osdp_sys.CARD_FMT_ASCII

class Capability:
    Unused = osdp_sys.CAP_UNUSED
    ContactStatusMonitoring = osdp_sys.CAP_CONTACT_STATUS_MONITORING
    OutputControl = osdp_sys.CAP_OUTPUT_CONTROL
    CardDataFormat = osdp_sys.CAP_CARD_DATA_FORMAT
    LEDControl = osdp_sys.CAP_READER_LED_CONTROL
    AudibleControl = osdp_sys.CAP_READER_AUDIBLE_OUTPUT
    TextOutput = osdp_sys.CAP_READER_TEXT_OUTPUT
    TimeKeeping = osdp_sys.CAP_TIME_KEEPING
    CheckCharacter = osdp_sys.CAP_CHECK_CHARACTER_SUPPORT
    CommunicationSecurity = osdp_sys.CAP_COMMUNICATION_SECURITY
    ReceiveBufferSize = osdp_sys.CAP_RECEIVE_BUFFERSIZE
    CombinedMessageSize = osdp_sys.CAP_LARGEST_COMBINED_MESSAGE_SIZE
    SmartCard = osdp_sys.CAP_SMART_CARD_SUPPORT
    Readers = osdp_sys.CAP_READERS
    Biometrics = osdp_sys.CAP_BIOMETRICS
