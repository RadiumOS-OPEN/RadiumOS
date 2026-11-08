// OpenAL and Vorbis stand-ins: AssaultCube runs without a sound device (alcOpenDevice fails, so no audio code is reached).
#include <AL/al.h>
#include <AL/alc.h>
#include <vorbis/vorbisfile.h>
#include <stddef.h>

extern "C" {

ALCdevice *alcOpenDevice(const ALCchar *) { return NULL; }
ALCboolean alcCloseDevice(ALCdevice *) { return ALC_TRUE; }
ALCcontext *alcCreateContext(ALCdevice *, const ALCint *) { return NULL; }
ALCboolean alcMakeContextCurrent(ALCcontext *) { return ALC_FALSE; }
void alcDestroyContext(ALCcontext *) {}
void alcSuspendContext(ALCcontext *) {}
void alcProcessContext(ALCcontext *) {}
ALCenum alcGetError(ALCdevice *) { return ALC_NO_ERROR; }
const ALCchar *alcGetString(ALCdevice *, ALCenum) { return ""; }
ALCboolean alcIsExtensionPresent(ALCdevice *, const ALCchar *) { return ALC_FALSE; }
ALboolean alIsExtensionPresent(const ALchar *) { return AL_FALSE; }
void *alGetProcAddress(const ALchar *) { return NULL; }
ALenum alGetError(void) { return AL_NO_ERROR; }
const ALchar *alGetString(ALenum) { return ""; }
void alDistanceModel(ALenum) {}
void alListenerf(ALenum, ALfloat) {}
void alListenerfv(ALenum, const ALfloat *) {}
void alGenBuffers(ALsizei, ALuint *) {}
void alDeleteBuffers(ALsizei, const ALuint *) {}
ALboolean alIsBuffer(ALuint) { return AL_FALSE; }
void alBufferData(ALuint, ALenum, const ALvoid *, ALsizei, ALsizei) {}
void alGenSources(ALsizei, ALuint *) {}
void alDeleteSources(ALsizei, const ALuint *) {}
ALboolean alIsSource(ALuint) { return AL_FALSE; }
void alSourcef(ALuint, ALenum, ALfloat) {}
void alSource3f(ALuint, ALenum, ALfloat, ALfloat, ALfloat) {}
void alSourcei(ALuint, ALenum, ALint) {}
void alGetSourcef(ALuint, ALenum, ALfloat *v) { *v = 0; }
void alGetSourcefv(ALuint, ALenum, ALfloat *v) { v[0] = v[1] = v[2] = 0; }
void alGetSourcei(ALuint, ALenum, ALint *v) { *v = 0; }
void alSourcePlay(ALuint) {}
void alSourceStop(ALuint) {}
void alSourceRewind(ALuint) {}
void alSourceQueueBuffers(ALuint, ALsizei, const ALuint *) {}
void alSourceUnqueueBuffers(ALuint, ALsizei, ALuint *) {}

int ov_clear(OggVorbis_File *) { return 0; }
int ov_open_callbacks(void *, OggVorbis_File *, char *, long, ov_callbacks) { return -1; }
vorbis_info *ov_info(OggVorbis_File *, int) { return NULL; }
double ov_time_total(OggVorbis_File *, int) { return 0; }
long ov_read(OggVorbis_File *, char *, int, int, int, int, int *) { return 0; }
int ov_pcm_seek(OggVorbis_File *, ogg_int64_t) { return -1; }
int ov_time_seek_page(OggVorbis_File *, double) { return -1; }

}
