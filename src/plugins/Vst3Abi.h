// Minimal, self-written declarations of the VST3 binary interface (COM-style vtables) needed to host plugins on
// Linux. Written from the public ABI description; contains no SDK code. Only the subset used by the host is declared,
// but each interface keeps the complete method order so vtable slots line up with the real thing.
#pragma once
#include <cstdint>
#include <cstring>

namespace vst3 {

using tresult = int32_t;
using int32 = int32_t;
using int64 = int64_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int16 = int16_t;
using uint16 = uint16_t;
using int8 = int8_t;
using uint8 = uint8_t;
using TBool = uint8_t;
using TChar = char16_t;
using FIDString = const char*;
using String128 = TChar[128];
using ParamID = uint32;
using ParamValue = double;
using SpeakerArrangement = uint64;
using MediaType = int32;
using BusDirection = int32;
using BusType = int32;
using UnitID = int32;
using NoteExpressionTypeID = uint32;
using NoteExpressionValue = double;
using CtrlNumber = int16;
using Sample32 = float;
using Sample64 = double;
using TUID = char[16];

enum : tresult {
    kNoInterface = -1,
    kResultOk = 0,
    kResultTrue = 0,
    kResultFalse = 1,
    kInvalidArgument = 2,
    kNotImplemented = 3,
    kInternalError = 4,
    kNotInitialized = 5,
    kOutOfMemory = 6
};
enum MediaTypes : int32 { kAudio = 0, kEvent = 1 };
enum BusDirections : int32 { kInput = 0, kOutput = 1 };
enum BusTypes : int32 { kMain = 0, kAux = 1 };
enum BusFlags : uint32 { kDefaultActive = 1 };
enum IoModes : int32 { kSimple = 0, kAdvanced = 1, kOfflineProcessing = 2 };
enum ProcessModes : int32 { kRealtime = 0, kPrefetch = 1, kOffline = 2 };
enum SymbolicSampleSizes : int32 { kSample32 = 0, kSample64 = 1 };
constexpr SpeakerArrangement kSpeakerL = 1ull << 0, kSpeakerR = 1ull << 1, kSpeakerM = 1ull << 19;
constexpr SpeakerArrangement kStereo = kSpeakerL | kSpeakerR, kMono = kSpeakerM;
enum RestartFlags : int32 {
    kReloadComponent = 1 << 0,
    kIoChanged = 1 << 1,
    kParamValuesChanged = 1 << 2,
    kLatencyChanged = 1 << 3,
    kParamTitlesChanged = 1 << 4,
    kMidiCCAssignmentChanged = 1 << 5
};
enum ParameterFlags : int32 {
    kCanAutomate = 1 << 0,
    kIsReadOnly = 1 << 1,
    kIsWrapAround = 1 << 2,
    kIsList = 1 << 3,
    kIsHidden = 1 << 4,
    kIsProgramChange = 1 << 15,
    kIsBypass = 1 << 16
};
enum SeekMode : int32 { kIBSeekSet = 0, kIBSeekCur = 1, kIBSeekEnd = 2 };
enum ClassCardinality : int32 { kManyInstances = 0x7FFFFFFF };
enum EventTypes : uint16 {
    kNoteOnEvent = 0,
    kNoteOffEvent = 1,
    kDataEvent = 2,
    kPolyPressureEvent = 3,
    kNoteExpressionValueEvent = 4,
    kNoteExpressionTextEvent = 5,
    kChordEvent = 6,
    kScaleEvent = 7,
    kLegacyMIDICCOutEvent = 65535
};
enum EventFlags : uint16 { kIsLive = 1 << 0 };
enum ProcessContextStates : uint32 {
    kPlaying = 1 << 1,
    kCycleActive = 1 << 2,
    kRecording = 1 << 3,
    kSystemTimeValid = 1 << 8,
    kContTimeValid = 1 << 17,
    kProjectTimeMusicValid = 1 << 9,
    kBarPositionValid = 1 << 11,
    kCycleValid = 1 << 12,
    kTempoValid = 1 << 10,
    kTimeSigValid = 1 << 13,
    kChordValid = 1 << 18,
    kSmpteValid = 1 << 14,
    kClockValid = 1 << 15
};

// Interface IDs (non-COM byte order used on Linux/macOS)
struct Uid {
    TUID id;
};
constexpr Uid makeUid(uint32 l1, uint32 l2, uint32 l3, uint32 l4) {
    return Uid{{(char)((l1 >> 24) & 0xff), (char)((l1 >> 16) & 0xff), (char)((l1 >> 8) & 0xff), (char)(l1 & 0xff),
        (char)((l2 >> 24) & 0xff), (char)((l2 >> 16) & 0xff), (char)((l2 >> 8) & 0xff), (char)(l2 & 0xff), (char)((l3 >> 24) & 0xff),
        (char)((l3 >> 16) & 0xff), (char)((l3 >> 8) & 0xff), (char)(l3 & 0xff), (char)((l4 >> 24) & 0xff), (char)((l4 >> 16) & 0xff),
        (char)((l4 >> 8) & 0xff), (char)(l4 & 0xff)}};
}
inline bool uidEq(const TUID a, const TUID b) { return std::memcmp(a, b, 16) == 0; }

constexpr Uid FUnknown_iid = makeUid(0x00000000, 0x00000000, 0xC0000000, 0x00000046);
constexpr Uid IPluginFactory_iid = makeUid(0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
constexpr Uid IPluginFactory2_iid = makeUid(0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
constexpr Uid IPluginFactory3_iid = makeUid(0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931);
constexpr Uid IPluginBase_iid = makeUid(0x22888DDB, 0x156E45AE, 0x8358B348, 0x08190625);
constexpr Uid IComponent_iid = makeUid(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
constexpr Uid IAudioProcessor_iid = makeUid(0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
constexpr Uid IEditController_iid = makeUid(0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
constexpr Uid IEditController2_iid = makeUid(0x7F4EFE59, 0xF3204967, 0xAC27A3AE, 0xAFB63038);
constexpr Uid IConnectionPoint_iid = makeUid(0x70A4156F, 0x6E6E4026, 0x989148BF, 0xAA60D8D1);
constexpr Uid IMessage_iid = makeUid(0x936F033B, 0xC6C047DB, 0xBB0882F8, 0x13C1E613);
constexpr Uid IAttributeList_iid = makeUid(0x1E5F0AEB, 0xCC7F4533, 0xA2544011, 0x38AD5EE4);
constexpr Uid IHostApplication_iid = makeUid(0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5);
constexpr Uid IComponentHandler_iid = makeUid(0x93A0BEA3, 0x0BD045DB, 0x8E890B0C, 0xC1E46AC6);
constexpr Uid IComponentHandler2_iid = makeUid(0xF040B4B3, 0xA36045EC, 0xABCDC045, 0xB4D5A2CC);
constexpr Uid IParamValueQueue_iid = makeUid(0x01263A18, 0xED074F6F, 0x98C9D356, 0x4686F9BA);
constexpr Uid IParameterChanges_iid = makeUid(0xA4779663, 0x0BB64A56, 0xB44384A8, 0x466FEB9D);
constexpr Uid IEventList_iid = makeUid(0x3A2C4214, 0x346349FE, 0xB2C4F397, 0xB9695A44);
constexpr Uid IBStream_iid = makeUid(0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B);
constexpr Uid IMidiMapping_iid = makeUid(0xDF0FF9F7, 0x49B74669, 0xB63AB732, 0x7ADBF5E5);
constexpr Uid IPlugView_iid = makeUid(0x5BC32507, 0xD06049EA, 0xA6151B52, 0x2B755B29);
constexpr Uid IPlugFrame_iid = makeUid(0x367FAF01, 0xAFA94693, 0x8D4DA2A0, 0xED0882A3);
constexpr Uid IRunLoop_iid = makeUid(0x18C35366, 0x97764F1A, 0x9C5B8385, 0x7A871389);
constexpr Uid IEventHandler_iid = makeUid(0x561E65C9, 0x13A0496F, 0x813A2C35, 0x654D7983);
constexpr Uid ITimerHandler_iid = makeUid(0x10BDD94F, 0x41424774, 0x821FAD8F, 0xECA72CA9);
constexpr Uid IProcessContextRequirements_iid = makeUid(0x2A654303, 0xEF764E3D, 0x95B5FE83, 0x730EF6D0);
constexpr Uid IPlugViewContentScaleSupport_iid = makeUid(0x65ED9690, 0x8AC44525, 0x8AADEF7A, 0x72EA703F);
constexpr Uid IUnitInfo_iid = makeUid(0x3D4BD6B5, 0x913A4FD2, 0xA886E768, 0xA5EB92C1);

constexpr const char* kVstAudioEffectClass = "Audio Module Class";
constexpr const char* kVstComponentControllerClass = "Component Controller Class";
constexpr const char* kPlatformTypeX11EmbedWindowID = "X11EmbedWindowID";
constexpr const char* kEditorViewType = "editor";

// ---- structs (layouts match the SDK's packed definitions on 64-bit Linux)
struct PFactoryInfo {
    char vendor[64];
    char url[256];
    char email[128];
    int32 flags;
};
struct PClassInfo {
    TUID cid;
    int32 cardinality;
    char category[32];
    char name[64];
};
struct PClassInfo2 {
    TUID cid;
    int32 cardinality;
    char category[32];
    char name[64];
    uint32 classFlags;
    char subCategories[128];
    char vendor[64];
    char version[64];
    char sdkVersion[64];
};
struct PClassInfoW {
    TUID cid;
    int32 cardinality;
    char category[32];
    TChar name[64];
    uint32 classFlags;
    char subCategories[128];
    TChar vendor[64];
    TChar version[64];
    TChar sdkVersion[64];
};
struct BusInfo {
    MediaType mediaType;
    BusDirection direction;
    int32 channelCount;
    String128 name;
    BusType busType;
    uint32 flags;
};
struct RoutingInfo {
    MediaType mediaType;
    int32 busIndex;
    int32 channel;
};
struct ProcessSetup {
    int32 processMode;
    int32 symbolicSampleSize;
    int32 maxSamplesPerBlock;
    double sampleRate;
};
struct AudioBusBuffers {
    int32 numChannels;
    uint64 silenceFlags;
    union {
        Sample32** channelBuffers32;
        Sample64** channelBuffers64;
    };
};
struct Chord {
    int8 keyNote;
    int8 rootNote;
    int16 chordMask;
};
struct FrameRate {
    uint32 framesPerSecond;
    uint32 flags;
};
struct ProcessContext {
    uint32 state;
    double sampleRate;
    int64 projectTimeSamples;
    int64 systemTime;
    int64 continousTimeSamples;
    double projectTimeMusic;
    double barPositionMusic;
    double cycleStartMusic;
    double cycleEndMusic;
    double tempo;
    int32 timeSigNumerator;
    int32 timeSigDenominator;
    Chord chord;
    int32 smpteOffsetSubframes;
    FrameRate frameRate;
    int32 samplesToNextClock;
};
struct NoteOnEvent {
    int16 channel;
    int16 pitch;
    float tuning;
    float velocity;
    int32 length;
    int32 noteId;
};
struct NoteOffEvent {
    int16 channel;
    int16 pitch;
    float velocity;
    int32 noteId;
    float tuning;
};
struct DataEvent {
    uint32 size;
    uint32 type;
    const uint8* bytes;
};
struct PolyPressureEvent {
    int16 channel;
    int16 pitch;
    float pressure;
    int32 noteId;
};
struct NoteExpressionValueEvent {
    NoteExpressionTypeID typeId;
    int32 noteId;
    NoteExpressionValue value;
};
struct NoteExpressionTextEvent {
    NoteExpressionTypeID typeId;
    int32 noteId;
    uint32 textLen;
    const TChar* text;
};
struct ChordEvent {
    int16 root;
    int16 bassNote;
    int16 mask;
    uint16 textLen;
    const TChar* text;
};
struct ScaleEvent {
    int16 root;
    uint16 mask;
    uint16 textLen;
    const TChar* text;
};
struct LegacyMIDICCOutEvent {
    uint8 controlNumber;
    int8 channel;
    int8 value;
    int8 value2;
};
struct Event {
    int32 busIndex;
    int32 sampleOffset;
    double ppqPosition;
    uint16 flags;
    uint16 type;
    union {
        NoteOnEvent noteOn;
        NoteOffEvent noteOff;
        DataEvent data;
        PolyPressureEvent polyPressure;
        NoteExpressionValueEvent noteExpressionValue;
        NoteExpressionTextEvent noteExpressionText;
        ChordEvent chord;
        ScaleEvent scale;
        LegacyMIDICCOutEvent midiCCOut;
    };
};
struct ParameterInfo {
    ParamID id;
    String128 title;
    String128 shortTitle;
    String128 units;
    int32 stepCount;
    ParamValue defaultNormalizedValue;
    UnitID unitId;
    int32 flags;
};
struct ViewRect {
    int32 left;
    int32 top;
    int32 right;
    int32 bottom;
    int32 getWidth() const { return right - left; }
    int32 getHeight() const { return bottom - top; }
};

// ---- interfaces (pure virtual; NO destructors so the vtable matches)
struct FUnknown {
    virtual tresult queryInterface(const TUID iid, void** obj) = 0;
    virtual uint32 addRef() = 0;
    virtual uint32 release() = 0;
};
struct IBStream : FUnknown {
    virtual tresult read(void* buffer, int32 numBytes, int32* numBytesRead) = 0;
    virtual tresult write(void* buffer, int32 numBytes, int32* numBytesWritten) = 0;
    virtual tresult seek(int64 pos, int32 mode, int64* result) = 0;
    virtual tresult tell(int64* pos) = 0;
};
struct IPluginBase : FUnknown {
    virtual tresult initialize(FUnknown* context) = 0;
    virtual tresult terminate() = 0;
};
struct IPluginFactory : FUnknown {
    virtual tresult getFactoryInfo(PFactoryInfo* info) = 0;
    virtual int32 countClasses() = 0;
    virtual tresult getClassInfo(int32 index, PClassInfo* info) = 0;
    virtual tresult createInstance(FIDString cid, FIDString iid, void** obj) = 0;
};
struct IPluginFactory2 : IPluginFactory {
    virtual tresult getClassInfo2(int32 index, PClassInfo2* info) = 0;
};
struct IPluginFactory3 : IPluginFactory2 {
    virtual tresult getClassInfoUnicode(int32 index, PClassInfoW* info) = 0;
    virtual tresult setHostContext(FUnknown* context) = 0;
};
struct IComponent : IPluginBase {
    virtual tresult getControllerClassId(TUID classId) = 0;
    virtual tresult setIoMode(int32 mode) = 0;
    virtual int32 getBusCount(MediaType type, BusDirection dir) = 0;
    virtual tresult getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) = 0;
    virtual tresult getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) = 0;
    virtual tresult activateBus(MediaType type, BusDirection dir, int32 index, TBool state) = 0;
    virtual tresult setActive(TBool state) = 0;
    virtual tresult setState(IBStream* state) = 0;
    virtual tresult getState(IBStream* state) = 0;
};
struct IParamValueQueue : FUnknown {
    virtual ParamID getParameterId() = 0;
    virtual int32 getPointCount() = 0;
    virtual tresult getPoint(int32 index, int32& sampleOffset, ParamValue& value) = 0;
    virtual tresult addPoint(int32 sampleOffset, ParamValue value, int32& index) = 0;
};
struct IParameterChanges : FUnknown {
    virtual int32 getParameterCount() = 0;
    virtual IParamValueQueue* getParameterData(int32 index) = 0;
    virtual IParamValueQueue* addParameterData(const ParamID& id, int32& index) = 0;
};
struct IEventList : FUnknown {
    virtual int32 getEventCount() = 0;
    virtual tresult getEvent(int32 index, Event& e) = 0;
    virtual tresult addEvent(Event& e) = 0;
};
struct ProcessData {
    int32 processMode;
    int32 symbolicSampleSize;
    int32 numSamples;
    int32 numInputs;
    int32 numOutputs;
    AudioBusBuffers* inputs;
    AudioBusBuffers* outputs;
    IParameterChanges* inputParameterChanges;
    IParameterChanges* outputParameterChanges;
    IEventList* inputEvents;
    IEventList* outputEvents;
    ProcessContext* processContext;
};
struct IAudioProcessor : FUnknown {
    virtual tresult setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) = 0;
    virtual tresult getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) = 0;
    virtual tresult canProcessSampleSize(int32 symbolicSampleSize) = 0;
    virtual uint32 getLatencySamples() = 0;
    virtual tresult setupProcessing(ProcessSetup& setup) = 0;
    virtual tresult setProcessing(TBool state) = 0;
    virtual tresult process(ProcessData& data) = 0;
    virtual uint32 getTailSamples() = 0;
};
struct IProcessContextRequirements : FUnknown {
    virtual uint32 getProcessContextRequirements() = 0;
};
struct IPlugFrame;
struct IPlugView : FUnknown {
    virtual tresult isPlatformTypeSupported(FIDString type) = 0;
    virtual tresult attached(void* parent, FIDString type) = 0;
    virtual tresult removed() = 0;
    virtual tresult onWheel(float distance) = 0;
    virtual tresult onKeyDown(char16_t key, int16 keyCode, int16 modifiers) = 0;
    virtual tresult onKeyUp(char16_t key, int16 keyCode, int16 modifiers) = 0;
    virtual tresult getSize(ViewRect* size) = 0;
    virtual tresult onSize(ViewRect* newSize) = 0;
    virtual tresult onFocus(TBool state) = 0;
    virtual tresult setFrame(IPlugFrame* frame) = 0;
    virtual tresult canResize() = 0;
    virtual tresult checkSizeConstraint(ViewRect* rect) = 0;
};
struct IPlugFrame : FUnknown {
    virtual tresult resizeView(IPlugView* view, ViewRect* newSize) = 0;
};
struct IPlugViewContentScaleSupport : FUnknown {
    virtual tresult setContentScaleFactor(float factor) = 0;
};
struct IComponentHandler : FUnknown {
    virtual tresult beginEdit(ParamID id) = 0;
    virtual tresult performEdit(ParamID id, ParamValue valueNormalized) = 0;
    virtual tresult endEdit(ParamID id) = 0;
    virtual tresult restartComponent(int32 flags) = 0;
};
struct IComponentHandler2 : FUnknown {
    virtual tresult setDirty(TBool state) = 0;
    virtual tresult requestOpenEditor(FIDString name) = 0;
    virtual tresult startGroupEdit() = 0;
    virtual tresult finishGroupEdit() = 0;
};
struct IEditController : IPluginBase {
    virtual tresult setComponentState(IBStream* state) = 0;
    virtual tresult setState(IBStream* state) = 0;
    virtual tresult getState(IBStream* state) = 0;
    virtual int32 getParameterCount() = 0;
    virtual tresult getParameterInfo(int32 paramIndex, ParameterInfo& info) = 0;
    virtual tresult getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) = 0;
    virtual tresult getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) = 0;
    virtual ParamValue normalizedParamToPlain(ParamID id, ParamValue valueNormalized) = 0;
    virtual ParamValue plainParamToNormalized(ParamID id, ParamValue plainValue) = 0;
    virtual ParamValue getParamNormalized(ParamID id) = 0;
    virtual tresult setParamNormalized(ParamID id, ParamValue value) = 0;
    virtual tresult setComponentHandler(IComponentHandler* handler) = 0;
    virtual IPlugView* createView(FIDString name) = 0;
};
struct IEditController2 : FUnknown {
    virtual tresult setKnobMode(int32 mode) = 0;
    virtual tresult openHelp(TBool onlyCheck) = 0;
    virtual tresult openAboutBox(TBool onlyCheck) = 0;
};
struct IMidiMapping : FUnknown {
    virtual tresult getMidiControllerAssignment(int32 busIndex, int16 channel, CtrlNumber midiControllerNumber, ParamID& id) = 0;
};
struct IAttributeList : FUnknown {
    using AttrID = const char*;
    virtual tresult setInt(AttrID id, int64 value) = 0;
    virtual tresult getInt(AttrID id, int64& value) = 0;
    virtual tresult setFloat(AttrID id, double value) = 0;
    virtual tresult getFloat(AttrID id, double& value) = 0;
    virtual tresult setString(AttrID id, const TChar* string) = 0;
    virtual tresult getString(AttrID id, TChar* string, uint32 sizeInBytes) = 0;
    virtual tresult setBinary(AttrID id, const void* data, uint32 sizeInBytes) = 0;
    virtual tresult getBinary(AttrID id, const void*& data, uint32& sizeInBytes) = 0;
};
struct IMessage : FUnknown {
    virtual FIDString getMessageID() = 0;
    virtual void setMessageID(FIDString id) = 0;
    virtual IAttributeList* getAttributes() = 0;
};
struct IConnectionPoint : FUnknown {
    virtual tresult connect(IConnectionPoint* other) = 0;
    virtual tresult disconnect(IConnectionPoint* other) = 0;
    virtual tresult notify(IMessage* message) = 0;
};
struct IHostApplication : FUnknown {
    virtual tresult getName(String128 name) = 0;
    virtual tresult createInstance(TUID cid, TUID iid, void** obj) = 0;
};
// Linux run loop (host-provided through IPlugFrame::queryInterface)
using FileDescriptor = int;
using TimerInterval = uint64;
struct IEventHandler : FUnknown {
    virtual void onFDIsSet(FileDescriptor fd) = 0;
};
struct ITimerHandler : FUnknown {
    virtual void onTimer() = 0;
};
struct IRunLoop : FUnknown {
    virtual tresult registerEventHandler(IEventHandler* handler, FileDescriptor fd) = 0;
    virtual tresult unregisterEventHandler(IEventHandler* handler) = 0;
    virtual tresult registerTimer(ITimerHandler* handler, TimerInterval milliseconds) = 0;
    virtual tresult unregisterTimer(ITimerHandler* handler) = 0;
};

using GetFactoryProc = IPluginFactory* (*)();
using ModuleEntryProc = bool (*)(void*);
using ModuleExitProc = bool (*)();

}   // namespace vst3
