"""Host tools for step 3 of the worn calibration, built on demand into
.build/ (git-ignored) with the system C compiler (cc / clang / gcc):

  aec_replay       this tree's modules/halo/src/audio_aec.c (FDAF build) +
                   aec_replay.c; tune sets through the real aec_tune C API
  aec_replay_0817  the 0.8.17 release's audio_aec.c (git tag 0.8.17), the
                   pre-fix reference set; skipped if the tag is missing
  spk_ref          drivers/audio/max98357a/speaker_protect.c + spk_ref.c,
                   the speaker chain as the AEC reference tap sees it

audio_aec.c is never edited: hook_source() inserts the replay hooks (the
shadow signal through the suppressor kernel, the processing path) into a
copy. A tool is rebuilt when any of its inputs changes (content hash).
"""
import hashlib
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ALIF = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", ".."))
HOST = os.path.join(HERE, "..", "host")
AEC_SRC = os.path.join(ALIF, "modules", "halo", "src", "audio_aec.c")
AEC_INC = os.path.join(ALIF, "modules", "halo", "include")
SPK_DIR = os.path.join(ALIF, "drivers", "audio", "max98357a")
BUILD = os.environ.get("AEC_CALIB_BUILD", os.path.join(HERE, ".build"))
# AEC_CALIB_REF_TAG: another reference tag (a missing one behaves as a clone without it)
REF_TAG = os.environ.get("AEC_CALIB_REF_TAG", "0.8.17")
CFLAGS = ["-O2", "-DCONFIG_HALO_AUDIO_AEC_TAPS=1024", "-DCONFIG_HALO_LOG_LEVEL=3",
          "-DCONFIG_HALO_AUDIO_AEC_FDAF=1", "-DCONFIG_HALO_AUDIO_AEC_FDAF_PARTS=3"]

_HOOK_DECL = """
/* ---- calibration replay hooks (inserted by calib_build.py) ---- */
const int16_t *host_shadow_in;
float host_shadow_out[FD_H];
int host_shadow_done;
int host_last_path;
static float host_shadow_hist[FD_N];
"""
_HOOK_SHADOW = """
		if (host_shadow_in) {
			memmove(host_shadow_hist, host_shadow_hist + FD_H,
				(FD_N - FD_H) * sizeof(float));
			for (uint32_t i = 0; i < FD_H; i++) {
				host_shadow_hist[FD_N - FD_H + i] =
					(float)host_shadow_in[i] * (1.0f / 32768.0f);
			}
			for (uint32_t i = 0; i < FD_H; i++) {
				const float *r = host_shadow_hist + FD_N - FD_H + i;
				float acc = 0.0f;

				for (uint32_t j = 0; j < AEC_SUP_KLEN; j++) {
					acc += sup.kc[j] * r[-(int32_t)j];
				}
				host_shadow_out[i] = acc * 32768.0f;
			}
			host_shadow_done = 1;
		}
"""


def hook_source(s):
    """audio_aec.c text -> the same with the replay hooks. Outputs at any tune
    set are bit-identical to the unhooked source (the hooks only read)."""
    def one(a, b):
        nonlocal s
        if s.count(a) != 1:
            raise RuntimeError(f"hook anchor not found exactly once: {a[:60]!r}")
        s = s.replace(a, b)
    one("static atomic_t sup_enabled = ATOMIC_INIT(1);\n",
        "static atomic_t sup_enabled = ATOMIC_INIT(1);\n" + _HOOK_DECL)
    # every suppressor history wipe also wipes the shadow's history
    wipe = "memset(sup.res_hist, 0, sizeof(sup.res_hist));"
    if wipe not in s:
        raise RuntimeError("hook anchor not found: res_hist wipe")
    s = s.replace(wipe, wipe + " memset(host_shadow_hist, 0, sizeof(host_shadow_hist));")
    one("\t\t\tpcm[i] = (int16_t)out;\n\t\t}\n\t}\n\n\tfd.frame++;",
        "\t\t\tpcm[i] = (int16_t)out;\n\t\t}\n" + _HOOK_SHADOW + "\t}\n\n\tfd.frame++;")
    one("memset(pcm, 0, AEC_MAX_BLOCK * sizeof(int16_t));\n\t\t\thave_held = true;",
        "memset(pcm, 0, AEC_MAX_BLOCK * sizeof(int16_t));\n\t\t\thave_held = true;\n"
        "\t\t\thost_last_path = 1;")
    one("\tuint32_t t_start = k_cyc_to_us_floor32(k_cycle_get_32());\n\n\twhile (samples > 0) {",
        "\tuint32_t t_start = k_cyc_to_us_floor32(k_cycle_get_32());\n\n\thost_last_path = 2;\n"
        "\twhile (samples > 0) {")
    one("void audio_aec_process(int16_t *pcm, size_t samples, uint32_t sample_rate,\n"
        "\t\t       uint8_t channels)\n{",
        "void audio_aec_process(int16_t *pcm, size_t samples, uint32_t sample_rate,\n"
        "\t\t       uint8_t channels)\n{\n\thost_last_path = 0;")
    return s


def _cc():
    for c in (os.environ.get("CC"), "cc", "clang", "gcc"):
        if c and shutil.which(c):
            return c
    raise SystemExit("no C compiler found (need cc, clang or gcc; set CC)")


def _hash(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode())
    return h.hexdigest()[:16]


def _files_bytes(paths):
    out = []
    for p in paths:
        with open(p, "rb") as f:
            out.append(f.read())
    return out


def _host_stubs():
    out = []
    for root, _, fs in os.walk(os.path.join(HOST, "zephyr")):
        out += [os.path.join(root, f) for f in sorted(fs)]
    return sorted(out) + [os.path.join(HOST, "max98357a_audio.h")]


def _build(name, key, write_sources, cmd_fn):
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, name)
    stamp = exe + ".key"
    if os.path.exists(exe) and os.path.exists(stamp) and open(stamp).read() == key:
        return exe
    wd = os.path.join(BUILD, name + ".src")
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    write_sources(wd)
    r = subprocess.run(cmd_fn(wd, exe + ".part"), capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"building {name} failed:\n{r.stderr[-3000:]}")
    os.replace(exe + ".part", exe)
    open(stamp, "w").write(key)
    return exe


def aec_replay():
    src = open(AEC_SRC).read()
    drv = os.path.join(HERE, "aec_replay.c")
    key = _hash(src, *_files_bytes([drv, os.path.join(AEC_INC, "halo", "audio_aec.h")] + _host_stubs()),
                *CFLAGS)

    def ws(wd):
        open(os.path.join(wd, "audio_aec_hooked.c"), "w").write(hook_source(src))

    return _build("aec_replay", key, ws, lambda wd, out: [
        _cc(), *CFLAGS, "-I", HOST, "-I", AEC_INC, "-o", out,
        os.path.join(wd, "audio_aec_hooked.c"), drv, "-lm"])


def _git_show(path):
    r = subprocess.run(["git", "-C", ALIF, "show", f"{REF_TAG}:{path}"], capture_output=True)
    return r.stdout if r.returncode == 0 else None


def aec_replay_ref():
    """The 0.8.17 build, or None when the tag is not in this clone."""
    src = _git_show("modules/halo/src/audio_aec.c")
    hdr = _git_show("modules/halo/include/halo/audio_aec.h")
    if src is None or hdr is None:
        return None
    drv = os.path.join(HERE, "aec_replay.c")
    key = _hash(src, hdr, *_files_bytes([drv] + _host_stubs()), *CFLAGS)

    def ws(wd):
        os.makedirs(os.path.join(wd, "include", "halo"))
        open(os.path.join(wd, "include", "halo", "audio_aec.h"), "wb").write(hdr)
        open(os.path.join(wd, "audio_aec_hooked.c"), "w").write(hook_source(src.decode()))

    return _build("aec_replay_0817", key, ws, lambda wd, out: [
        _cc(), *CFLAGS, "-DREPLAY_NO_TUNE", "-I", HOST, "-I", os.path.join(wd, "include"),
        "-o", out, os.path.join(wd, "audio_aec_hooked.c"), drv, "-lm"])


def spk_ref():
    drv = os.path.join(HERE, "spk_ref.c")
    srcs = [os.path.join(SPK_DIR, "speaker_protect.c"), os.path.join(SPK_DIR, "speaker_protect.h"), drv]
    key = _hash(*_files_bytes(srcs))
    return _build("spk_ref", key, lambda wd: None, lambda wd, out: [
        _cc(), "-O2", "-I", SPK_DIR, "-o", out, drv, os.path.join(SPK_DIR, "speaker_protect.c"), "-lm"])


def build_all(verbose=False):
    t = {"aec_replay": aec_replay(), "aec_replay_0817": aec_replay_ref(), "spk_ref": spk_ref()}
    if verbose:
        for k, v in t.items():
            print(f"  {k}: {v or 'skipped (no ' + REF_TAG + ' tag in this clone)'}")
    return t


if __name__ == "__main__":
    build_all(verbose=True)
