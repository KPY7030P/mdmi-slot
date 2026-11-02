// Headless VST3 host for testing the wrapped plugin without a DAW.
//
//   vst3_host <plugin.vst3> <out_prefix> <rate> <block> <offline|realtime>
//
// Plays notes through VST3 events and sends CC7 through IMidiMapping, the way a DAW does,
// so note 64 must come out quieter than note 60. Log format matches clap_host.
#define INIT_CLASS_IID
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#undef INIT_CLASS_IID

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

static const int SECONDS = 6;

class TestHost : public IHostApplication
{
public:
	tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
	{
		if (FUnknownPrivate::iidEqual(iid, IHostApplication::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
			*obj = this;
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	tresult PLUGIN_API getName(String128 name) override
	{
		static const char16_t host_name[] = u"mdmi-test-host";
		memcpy(name, host_name, sizeof(host_name));
		return kResultOk;
	}
	tresult PLUGIN_API createInstance(TUID, TUID, void **obj) override { *obj = nullptr; return kResultFalse; }
};

class EventQueue : public IEventList
{
public:
	std::vector<Event> events;
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override { *obj = nullptr; return kNoInterface; }
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	int32 PLUGIN_API getEventCount() override { return (int32)events.size(); }
	tresult PLUGIN_API getEvent(int32 index, Event &e) override { e = events[index]; return kResultOk; }
	tresult PLUGIN_API addEvent(Event &e) override { events.push_back(e); return kResultOk; }
};

class SingleParamQueue : public IParamValueQueue
{
public:
	ParamID id = 0;
	int32 offset = 0;
	ParamValue value = 0;
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override { *obj = nullptr; return kNoInterface; }
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	ParamID PLUGIN_API getParameterId() override { return id; }
	int32 PLUGIN_API getPointCount() override { return 1; }
	tresult PLUGIN_API getPoint(int32, int32 &sampleOffset, ParamValue &v) override { sampleOffset = offset; v = value; return kResultOk; }
	tresult PLUGIN_API addPoint(int32, ParamValue, int32 &) override { return kResultFalse; }
};

class ParamChanges : public IParameterChanges
{
public:
	SingleParamQueue queue;
	bool used = false;
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override { *obj = nullptr; return kNoInterface; }
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	int32 PLUGIN_API getParameterCount() override { return used ? 1 : 0; }
	IParamValueQueue *PLUGIN_API getParameterData(int32) override { return used ? &queue : nullptr; }
	IParamValueQueue *PLUGIN_API addParameterData(const ParamID &, int32 &) override { return nullptr; }
};

static void write_le(FILE *f, uint32_t value, int bytes)
{
	for (int i = 0; i < bytes; i++) {
		fputc(value >> (8 * i) & 0xFF, f);
	}
}

static void write_wav(const char *path, const std::vector<float> &left, const std::vector<float> &right, uint32_t rate)
{
	FILE *f = fopen(path, "wb");
	uint32_t frames = (uint32_t)left.size();
	uint32_t size = frames * 8;
	fwrite("RIFF", 1, 4, f);
	write_le(f, 36 + size, 4);
	fwrite("WAVEfmt ", 1, 8, f);
	write_le(f, 16, 4);
	write_le(f, 3, 2);
	write_le(f, 2, 2);
	write_le(f, rate, 4);
	write_le(f, rate * 8, 4);
	write_le(f, 8, 2);
	write_le(f, 32, 2);
	fwrite("data", 1, 4, f);
	write_le(f, size, 4);
	for (uint32_t i = 0; i < frames; i++) {
		fwrite(&left[i], 4, 1, f);
		fwrite(&right[i], 4, 1, f);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc != 6) {
		fprintf(stderr, "usage: %s <plugin.vst3> <out_prefix> <rate> <block> <offline|realtime>\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	uint32_t rate = (uint32_t)atoi(argv[3]);
	int32 block = atoi(argv[4]);
	bool offline = !strcmp(argv[5], "offline");

	HMODULE lib = LoadLibraryA(argv[1]);
	if (!lib) {
		fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
		return 1;
	}
	auto init_dll = (bool (*)())GetProcAddress(lib, "InitDll");
	auto exit_dll = (bool (*)())GetProcAddress(lib, "ExitDll");
	auto get_factory = (IPluginFactory * (PLUGIN_API *)())GetProcAddress(lib, "GetPluginFactory");
	if (!get_factory) {
		fprintf(stderr, "no GetPluginFactory export\n");
		return 1;
	}
	if (init_dll) {
		init_dll();
	}
	IPluginFactory *factory = get_factory();

	IPluginFactory2 *factory2 = nullptr;
	if (factory->queryInterface(IPluginFactory2::iid, (void **)&factory2) == kResultOk) {
		PClassInfo2 info2;
		factory2->getClassInfo2(0, &info2);
		printf("version=%s\n", info2.version);
	}

	TUID cid;
	bool found = false;
	for (int32 i = 0; i < factory->countClasses(); i++) {
		PClassInfo info;
		factory->getClassInfo(i, &info);
		printf("class %d: %s [%s]\n", i, info.name, info.category);
		if (!strcmp(info.category, kVstAudioEffectClass)) {
			memcpy(cid, info.cid, sizeof(TUID));
			found = true;
		}
	}
	if (!found) {
		fprintf(stderr, "no audio effect class\n");
		return 1;
	}

	IComponent *component = nullptr;
	if (factory->createInstance(cid, IComponent::iid, (void **)&component) != kResultOk || !component) {
		fprintf(stderr, "createInstance failed\n");
		return 1;
	}
	TestHost test_host;
	if (component->initialize(&test_host) != kResultOk) {
		fprintf(stderr, "initialize failed\n");
		return 1;
	}
	printf("audio_out_buses=%d event_in_buses=%d\n", component->getBusCount(kAudio, kOutput), component->getBusCount(kEvent, kInput));

	IAudioProcessor *processor = nullptr;
	IMidiMapping *mapping = nullptr;
	component->queryInterface(IAudioProcessor::iid, (void **)&processor);
	component->queryInterface(IMidiMapping::iid, (void **)&mapping);
	if (!processor) {
		fprintf(stderr, "no IAudioProcessor\n");
		return 1;
	}
	component->activateBus(kAudio, kOutput, 0, true);
	component->activateBus(kEvent, kInput, 0, true);
	SpeakerArrangement stereo = SpeakerArr::kStereo;
	processor->setBusArrangements(nullptr, 0, &stereo, 1);

	ProcessSetup setup = {};
	setup.processMode = offline ? kOffline : kRealtime;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = block;
	setup.sampleRate = rate;
	if (processor->setupProcessing(setup) != kResultOk) {
		fprintf(stderr, "setupProcessing failed\n");
		return 1;
	}
	component->setActive(true);
	processor->setProcessing(true);
	printf("latency=%u\n", processor->getLatencySamples());
	ParamID cc7_param = kNoParamId;
	if (mapping) {
		mapping->getMidiControllerAssignment(0, 0, 7, cc7_param);
	}
	// What a host lists as controls, and the knob Live would move by id.
	IEditController *controller = nullptr;
	component->queryInterface(IEditController::iid, (void **)&controller);
	int automatable = 0, hidden = 0;
	if (controller) {
		for (int32 i = 0; i < controller->getParameterCount(); i++) {
			ParameterInfo info = {};
			controller->getParameterInfo(i, info);
			char title[128] = "";
			for (int c = 0; c < 127 && info.title[c]; c++) {
				title[c] = (char)info.title[c];
			}
			if (info.flags & ParameterInfo::kCanAutomate) {
				automatable++;
				if (automatable <= 8) {
					printf("control %d: %s\n", automatable, title);
				}
				if (getenv("MDMI_TEST_PROGRAM") && !strcmp(title, "Preset")) {
					cc7_param = info.id;
				}
			}
			hidden += (info.flags & ParameterInfo::kIsHidden) != 0;
		}
	}
	printf("automatable=%d hidden=%d\n", automatable, hidden);
	printf("cc7_param=%u\n", (unsigned)cc7_param);

	// The window, attached the way a host attaches it, stays open while the sound runs.
	IPlugView *view = controller ? controller->createView(ViewType::kEditor) : nullptr;
	HWND view_parent = CreateWindowExA(0, "STATIC", "vst3_host", WS_POPUP, 0, 0, 960, 704, nullptr, nullptr, nullptr, nullptr);
	if (view) {
		ViewRect rect;
		view->getSize(&rect);
		bool attached = view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue && view->attached(view_parent, kPlatformTypeHWND) == kResultOk;
		printf("view=%dx%d attached=%d\n", rect.getWidth(), rect.getHeight(), attached);
	}

	struct NoteScript { double seconds; bool on; int16 pitch; };
	const NoteScript notes[] = {{3.0, true, 60}, {3.5, false, 60}, {4.0, true, 64}, {4.5, false, 64}};
	const double cc_seconds = 3.8;
	const uint32_t total = rate * SECONDS;
	std::vector<float> left(total + block), right(total + block);

	LARGE_INTEGER freq, start, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);
	size_t next = 0;
	bool cc_sent = false;
	for (uint32_t pos = 0; pos < total; pos += block) {
		EventQueue events;
		while (next < sizeof(notes) / sizeof(*notes) && (uint32_t)(notes[next].seconds * rate) < pos + block) {
			uint32_t at = (uint32_t)(notes[next].seconds * rate);
			Event e = {};
			e.sampleOffset = at - pos;
			if (notes[next].on) {
				e.type = Event::kNoteOnEvent;
				e.noteOn.pitch = notes[next].pitch;
				e.noteOn.velocity = 1.0f;
				e.noteOn.noteId = -1;
			} else {
				e.type = Event::kNoteOffEvent;
				e.noteOff.pitch = notes[next].pitch;
				e.noteOff.noteId = -1;
			}
			events.events.push_back(e);
			printf("EVENT %s %d sample=%u\n", notes[next].on ? "90" : "80", notes[next].pitch, at);
			next++;
		}
		ParamChanges changes;
		if (!cc_sent && cc7_param != kNoParamId && (uint32_t)(cc_seconds * rate) < pos + block) {
			changes.used = true;
			changes.queue.id = cc7_param;
			changes.queue.offset = (uint32_t)(cc_seconds * rate) - pos;
			changes.queue.value = (getenv("MDMI_TEST_PROGRAM") ? atoi(getenv("MDMI_TEST_PROGRAM")) : 90) / 127.0;
			cc_sent = true;
		}
		float *channels[2] = {left.data() + pos, right.data() + pos};
		AudioBusBuffers out = {};
		out.numChannels = 2;
		out.channelBuffers32 = channels;
		ProcessData data;
		data.processMode = setup.processMode;
		data.symbolicSampleSize = kSample32;
		data.numSamples = block;
		data.numOutputs = 1;
		data.outputs = &out;
		data.inputEvents = &events;
		data.inputParameterChanges = &changes;
		processor->process(data);

		MSG message;
		while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
			DispatchMessageA(&message);
		}
		if (!offline) {
			double due = (double)(pos + block) / rate;
			for (;;) {
				QueryPerformanceCounter(&now);
				double elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
				if (elapsed >= due) {
					break;
				}
				// Spin rather than Sleep: Sleep can oversleep by 15ms, and catching up afterwards
				// would call process() faster than real time, which no audio driver does.
				SwitchToThread();
			}
		}
	}
	QueryPerformanceCounter(&now);
	printf("wall=%.3fs audio=%ds\n", (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart, SECONDS);

	if (view) {
		view->removed();
		view->release();
	}
	DestroyWindow(view_parent);
	processor->setProcessing(false);
	component->setActive(false);
	component->terminate();
	left.resize(total);
	right.resize(total);
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s-0.wav", argv[2]);
	write_wav(path, left, right, rate);
	if (controller) {
		controller->release();
	}
	if (mapping) {
		mapping->release();
	}
	processor->release();
	component->release();
	factory->release();
	if (exit_dll) {
		exit_dll();
	}
	printf("clean_shutdown=1\n");
	fflush(stdout);
	return 0;
}
