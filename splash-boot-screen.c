/* splash-boot-screen - SplashOS boot screen: static logo + progress bar.
 *
 * The Splash Boot Manager plays its logo animation with splash-logo and
 * leaves the LAST frame on the framebuffer when it kexecs SplashOS. This
 * program paints that same frame at the same position and size, so the
 * handoff from the boot manager to SplashOS shows no jump, and adds a
 * rounded progress bar (no text, thin outline) below it.
 *
 * Pixel identity with the boot manager matters, so the geometry and the
 * resampler are copied verbatim from SplashBootManager/splash-logo.c:
 *   - logo height = LOGO_PERCENT (default 25) % of the screen height,
 *     width from the aspect ratio, centered, on black
 *   - box/tent resampling with 16.16 fixed-point weights
 * Keep them in sync if splash-logo.c changes.
 *
 * The asset is a SPLASHLOGO2 file with one frame (the animation's last
 * frame); `--extract IN OUT` makes one from the boot manager's animation.
 *
 * Progress comes from systemd: Manager.Progress (0..1) read over PID 1's
 * private socket (/run/systemd/private), which exists in the initrd and
 * before the D-Bus broker starts. The initrd fills 0-30% of the bar and
 * the real root 30-100%; the value reached is kept in /run (which survives
 * switch-root) so the bar never moves backwards between the two instances.
 *
 * Every frame is composed in memory and copied to the framebuffer in one
 * pass, so repainting the logo over an identical logo causes no flicker.
 * On exit the last picture stays on the screen until the next owner of the
 * display (GDM) takes over.
 *
 * Modes:
 *   splash-boot-screen                    run the boot screen until SIGTERM
 *   splash-boot-screen --restore-console  rebind fbcon + re-enable the cursor
 *   splash-boot-screen --extract IN OUT   last frame of IN -> 1-frame OUT
 *
 * Environment: FB0 (framebuffer device), LOGO (asset path), LOGO_PERCENT.
 * Test hooks: SPLASH_TEST_XRES/YRES fake the geometry (FB0 = plain file),
 * SPLASH_TEST_PROGRESS paints one picture at that progress and exits.
 */
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <systemd/sd-bus.h>

#define HDR_MAGIC	"SPLASHLOGO2"
#define HDR_SIZE	28
#define DEFAULT_LOGO	"/usr/share/splashos/boot-logo-static.raw"
#define DEFAULT_PERCENT	25
#define BPP		3	/* BGR24 */

#define PROGRESS_FILE	"/run/splash-boot-screen.progress"
#define INITRD_SHARE	0.30	/* part of the bar the initrd fills */
#define FRAME_NS	16000000L	/* ~60 fps */
#define POLL_FRAMES	3		/* ask systemd every 3rd frame */
#define LINGER_SEC	120		/* give up this long after 100% */

/* bar colours on black (0..255) */
#define OUTLINE_GREY	176
#define FILL_WHITE	255

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

/* ---- resampler: verbatim from splash-logo.c ------------------------- */

struct span {
	unsigned first;
	unsigned n;
	unsigned *weight;
};

static struct span *spans_build(unsigned src, unsigned dst)
{
	struct span *sp = calloc(dst, sizeof(*sp));
	if (!sp)
		return NULL;

	for (unsigned d = 0; d < dst; d++) {
		unsigned long long lo = (unsigned long long)d * src * 65536 / dst;
		unsigned long long hi = (unsigned long long)(d + 1) * src * 65536 / dst;
		unsigned first = (unsigned)(lo >> 16);
		unsigned last = (unsigned)((hi + 65535) >> 16);
		unsigned n;

		if (last > src)
			last = src;
		n = last - first;
		sp[d].weight = malloc((size_t)n * sizeof(unsigned));
		if (!sp[d].weight)
			return NULL;
		sp[d].first = first;
		sp[d].n = n;
		for (unsigned i = 0; i < n; i++) {
			unsigned long long s = (unsigned long long)(first + i) << 16;
			unsigned long long e = s + 65536;
			sp[d].weight[i] = (unsigned)((hi < e ? hi : e)
						   - (lo > s ? lo : s));
		}
	}
	return sp;
}

static void blit_frame(const unsigned char *src, unsigned sw,
		       unsigned char *fb, size_t scanline,
		       unsigned xoff, unsigned yoff,
		       const struct span *xs, const struct span *ys,
		       unsigned tw, unsigned th)
{
	for (unsigned y = 0; y < th; y++) {
		unsigned char *drow = fb + (size_t)(yoff + y) * scanline
				      + (size_t)xoff * 4;
		const struct span *spy = &ys[y];

		for (unsigned x = 0; x < tw; x++) {
			const struct span *spx = &xs[x];
			unsigned long long sb = 0, sg = 0, sr = 0, sw_sum = 0;
			unsigned char *d = drow + (size_t)x * 4;

			for (unsigned j = 0; j < spy->n; j++) {
				const unsigned char *srow = src
					+ (size_t)(spy->first + j) * sw * BPP;
				unsigned long long wy = spy->weight[j];

				for (unsigned i = 0; i < spx->n; i++) {
					const unsigned char *p = srow
						+ (size_t)(spx->first + i) * BPP;
					unsigned long long w = wy * spx->weight[i];

					sb += w * p[0];
					sg += w * p[1];
					sr += w * p[2];
					sw_sum += w;
				}
			}
			d[0] = (unsigned char)((sb + sw_sum / 2) / sw_sum);
			d[1] = (unsigned char)((sg + sw_sum / 2) / sw_sum);
			d[2] = (unsigned char)((sr + sw_sum / 2) / sw_sum);
			d[3] = 0;
		}
	}
}

/* ---- console handling ----------------------------------------------- */

/* (Un)bind fbcon. The vtconsole numbering is not stable, so match the
 * driver name like splash-logo does. */
static void fbcon_bind(int bind)
{
	DIR *dir = opendir("/sys/class/vtconsole");
	struct dirent *ent;
	char path[512], name[128];

	if (!dir)
		return;
	while ((ent = readdir(dir)) != NULL) {
		int fd, n;

		if (strncmp(ent->d_name, "vtcon", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/vtconsole/%s/name",
			 ent->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = (int)read(fd, name, sizeof(name) - 1);
		close(fd);
		if (n <= 0)
			continue;
		name[n] = '\0';
		if (strstr(name, "frame buffer") == NULL)
			continue;
		snprintf(path, sizeof(path), "/sys/class/vtconsole/%s/bind",
			 ent->d_name);
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			(void)!write(fd, bind ? "1" : "0", 1);
			close(fd);
		}
	}
	closedir(dir);
}

static void write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		(void)!write(fd, val, strlen(val));
		close(fd);
	}
}

/* Undo what the boot screen and the kernel command line did to the text
 * console: fbcon back on, cursor back on for newly opened VTs. */
static int restore_console(void)
{
	fbcon_bind(1);
	write_file("/sys/module/vt/parameters/global_cursor_default", "-1");
	return 0;
}

/* ---- asset ---------------------------------------------------------- */

struct logo {
	const unsigned char *map;
	size_t size;
	unsigned w, h, nframes;
};

static int logo_open(const char *path, struct logo *lg)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;

	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || st.st_size < HDR_SIZE) {
		close(fd);
		return -1;
	}
	lg->size = (size_t)st.st_size;
	lg->map = mmap(NULL, lg->size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (lg->map == MAP_FAILED)
		return -1;
	if (memcmp(lg->map, HDR_MAGIC, strlen(HDR_MAGIC)) != 0)
		goto bad;
	memcpy(&lg->w, lg->map + 12, 4);
	memcpy(&lg->h, lg->map + 16, 4);
	memcpy(&lg->nframes, lg->map + 20, 4);
	size_t fsize = (size_t)lg->w * lg->h * BPP;
	if (!lg->w || !lg->h || !lg->nframes || fsize / BPP / lg->h != lg->w ||
	    (unsigned long long)HDR_SIZE + (unsigned long long)fsize * lg->nframes
			> (unsigned long long)lg->size)
		goto bad;
	return 0;
bad:
	munmap((void *)lg->map, lg->size);
	return -1;
}

static const unsigned char *logo_frame(const struct logo *lg, unsigned f)
{
	return lg->map + HDR_SIZE + (size_t)f * lg->w * lg->h * BPP;
}

/* --extract IN OUT: write the LAST frame of IN as a one-frame asset. */
static int extract(const char *in, const char *out)
{
	struct logo lg;
	unsigned char hdr[HDR_SIZE];
	unsigned one = 1, zero = 0;
	FILE *f;

	if (logo_open(in, &lg) < 0) {
		fprintf(stderr, "splash-boot-screen: %s is not a SPLASHLOGO2 asset\n", in);
		return 1;
	}
	memcpy(hdr, lg.map, HDR_SIZE);
	memcpy(hdr + 20, &one, 4);
	memcpy(hdr + 24, &zero, 4);
	f = fopen(out, "wb");
	if (!f ||
	    fwrite(hdr, 1, HDR_SIZE, f) != HDR_SIZE ||
	    fwrite(logo_frame(&lg, lg.nframes - 1), (size_t)lg.w * lg.h * BPP, 1, f) != 1 ||
	    fclose(f) != 0) {
		perror(out);
		return 1;
	}
	return 0;
}

/* ---- progress bar --------------------------------------------------- */

struct bar {
	unsigned x, y, w, h;	/* outer box */
	float border;		/* outline width */
	float inset;		/* outline + black gap before the fill */
};

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Signed distance from (px,py) to a horizontal capsule (w >= h). */
static float sd_capsule(float px, float py, float x0, float y0, float w, float h)
{
	float r = h / 2.0f;
	float cx = clampf(px, x0 + r, x0 + w - r);
	float dx = px - cx, dy = py - (y0 + r);

	return sqrtf(dx * dx + dy * dy) - r;
}

static float coverage(float sd)
{
	return clampf(0.5f - sd, 0.0f, 1.0f);
}

/* Paint the bar region (plus a 1px margin) at progress p into buf. */
static void draw_bar(unsigned char *buf, size_t scanline,
		     const struct bar *b, float p)
{
	float ox = (float)b->x, oy = (float)b->y;
	float ow = (float)b->w, oh = (float)b->h;
	float ix = ox + b->inset, iy = oy + b->inset;
	float iw = ow - 2 * b->inset, ih = oh - 2 * b->inset;
	float fw = iw * clampf(p, 0.0f, 1.0f);
	/* The fill is a capsule too; below its own height it would be a
	 * squashed dot, so it starts as a full circle that fades in. */
	float fade = fw < ih ? fw / ih : 1.0f;
	if (fw < ih)
		fw = ih;

	for (unsigned y = b->y - 1; y <= b->y + b->h; y++) {
		unsigned char *row = buf + (size_t)y * scanline;

		for (unsigned x = b->x - 1; x <= b->x + b->w; x++) {
			float px = (float)x + 0.5f, py = (float)y + 0.5f;
			float outer = coverage(sd_capsule(px, py, ox, oy, ow, oh));
			float hole = coverage(sd_capsule(px, py, ox + b->border,
							 oy + b->border,
							 ow - 2 * b->border,
							 oh - 2 * b->border));
			float ring = outer - hole;
			float fill = p > 0.0f
				? coverage(sd_capsule(px, py, ix, iy, fw, ih)) * fade
				: 0.0f;
			float v = ring * OUTLINE_GREY + fill * FILL_WHITE;
			unsigned char c = (unsigned char)clampf(v + 0.5f, 0, 255);
			unsigned char *d = row + (size_t)x * 4;

			d[0] = d[1] = d[2] = c;
			d[3] = 0;
		}
	}
}

/* ---- systemd progress ----------------------------------------------- */

static sd_bus *bus_connect(void)
{
	sd_bus *bus = NULL;

	if (sd_bus_new(&bus) < 0)
		return NULL;
	if (sd_bus_set_address(bus, "unix:path=/run/systemd/private") < 0 ||
	    sd_bus_start(bus) < 0) {
		sd_bus_unref(bus);
		return NULL;
	}
	return bus;
}

/* Manager.Progress, or -1 when PID 1 cannot be asked (yet). */
static double systemd_progress(sd_bus **bus)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	double v = -1;

	if (!*bus)
		*bus = bus_connect();
	if (!*bus)
		return -1;
	if (sd_bus_get_property_trivial(*bus, NULL, "/org/freedesktop/systemd1",
					"org.freedesktop.systemd1.Manager",
					"Progress", &err, 'd', &v) < 0) {
		sd_bus_error_free(&err);
		*bus = sd_bus_flush_close_unref(*bus);
		return -1;
	}
	return v;
}

static double read_floor(void)
{
	FILE *f = fopen(PROGRESS_FILE, "re");
	double v = 0;

	if (f) {
		if (fscanf(f, "%lf", &v) != 1)
			v = 0;
		fclose(f);
	}
	return clampf((float)v, 0, 1);
}

static void write_floor(double v)
{
	FILE *f = fopen(PROGRESS_FILE ".tmp", "we");

	if (!f)
		return;
	fprintf(f, "%.4f\n", v);
	if (fclose(f) == 0)
		rename(PROGRESS_FILE ".tmp", PROGRESS_FILE);
}

/* ---- main ----------------------------------------------------------- */

static int open_fb(const char *path)
{
	/* In the initrd the unit can start before the GPU driver has
	 * registered /dev/fb0; wait up to 5 s for it. */
	for (int i = 0; i < 250; i++) {
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0 || errno != ENOENT)
			return fd;
		usleep(20000);
	}
	return -1;
}

static void sleep_frame(void)
{
	struct timespec ts = { 0, FRAME_NS };

	nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--restore-console") == 0)
		return restore_console();
	if (argc == 4 && strcmp(argv[1], "--extract") == 0)
		return extract(argv[2], argv[3]);

	const char *fbdev = getenv("FB0") ? : "/dev/fb0";
	const char *logo_path = getenv("LOGO") ? : DEFAULT_LOGO;
	int percent = getenv("LOGO_PERCENT") ? atoi(getenv("LOGO_PERCENT"))
					     : DEFAULT_PERCENT;
	int in_initrd = access("/etc/initrd-release", F_OK) == 0;
	double base = in_initrd ? 0.0 : INITRD_SHARE;
	double span = in_initrd ? INITRD_SHARE : 1.0 - INITRD_SHARE;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	struct logo lg;
	if (logo_open(logo_path, &lg) < 0)
		return 1;

	int fd = open_fb(fbdev);
	if (fd < 0)
		return 1;
	struct fb_var_screeninfo vinfo;
	memset(&vinfo, 0, sizeof(vinfo));
	if (getenv("SPLASH_TEST_XRES")) {
		/* test hook (as in splash-logo): fake geometry, FB0 is a file */
		vinfo.bits_per_pixel = 32;
		vinfo.xres = vinfo.xres_virtual = atoi(getenv("SPLASH_TEST_XRES"));
		vinfo.yres = vinfo.yres_virtual = atoi(getenv("SPLASH_TEST_YRES"));
	} else if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
		   vinfo.bits_per_pixel != 32) {
		fprintf(stderr, "splash-boot-screen: unsupported framebuffer (%ubpp)\n",
			vinfo.bits_per_pixel);
		return 1;
	}
	size_t scanline = (size_t)vinfo.xres_virtual * 4;
	size_t screen = scanline * vinfo.yres_virtual;

	/* logo geometry: identical to splash-logo.c */
	if (percent <= 0 || percent > 100)
		percent = DEFAULT_PERCENT;
	unsigned th = (unsigned)vinfo.yres * (unsigned)percent / 100u;
	unsigned tw = (unsigned)((unsigned long long)lg.w * th / lg.h);
	if (!tw || !th || tw > (unsigned)vinfo.xres) {
		tw = vinfo.xres;
		th = (unsigned)((unsigned long long)lg.h * tw / lg.w);
	}
	unsigned xoff = (vinfo.xres > tw) ? ((unsigned)vinfo.xres - tw) / 2u : 0;
	unsigned yoff = (vinfo.yres > th) ? ((unsigned)vinfo.yres - th) / 2u : 0;

	/* bar geometry, proportional to the logo: 3/4 of its width, 1/20 of
	 * its height thick, 1/6 of its height below it. */
	struct bar b;
	b.h = th / 20 < 6 ? 6 : th / 20;
	b.w = tw * 3 / 4;
	b.x = (vinfo.xres - b.w) / 2;
	b.y = yoff + th + th / 6;
	b.border = b.h >= 16 ? 2.0f : 1.0f;
	b.inset = 2 * b.border;
	int have_bar = b.w > 2 * b.h && b.x >= 1 &&
		       b.y + b.h + 1 < (unsigned)vinfo.yres;

	struct span *xs = spans_build(lg.w, tw);
	struct span *ys = spans_build(lg.h, th);
	unsigned char *back = calloc(1, screen);	/* black */
	if (!xs || !ys || !back)
		return 1;

	unsigned char *fb = mmap(NULL, screen, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		return 1;

	double shown = read_floor();
	double target = shown;

	/* test hook: paint one picture at a fixed progress and exit */
	if (getenv("SPLASH_TEST_PROGRESS")) {
		shown = atof(getenv("SPLASH_TEST_PROGRESS"));
		blit_frame(logo_frame(&lg, lg.nframes - 1), lg.w, back, scanline,
			   xoff, yoff, xs, ys, tw, th);
		if (have_bar)
			draw_bar(back, scanline, &b, (float)shown);
		memcpy(fb, back, screen);
		return 0;
	}

	/* compose the whole screen, then replace the framebuffer in one pass:
	 * the logo pixels the boot manager left behind are rewritten with
	 * identical values, so nothing visibly changes but the bar. */
	blit_frame(logo_frame(&lg, lg.nframes - 1), lg.w, back, scanline,
		   xoff, yoff, xs, ys, tw, th);
	if (have_bar)
		draw_bar(back, scanline, &b, (float)shown);
	fbcon_bind(0);
	memcpy(fb, back, screen);

	sd_bus *bus = NULL;
	unsigned frame = 0;
	time_t full_since = 0;

	while (!stop_requested) {
		if (frame++ % POLL_FRAMES == 0) {
			double p = systemd_progress(&bus);
			if (p >= 0) {
				double t = base + span * p;
				if (t > target)
					target = t;
			}
		}
		/* ease towards the target: quick to move, never backwards */
		double next = shown + (target - shown) * 0.12;
		if (target - next < 0.0005)
			next = target;
		if (have_bar && (unsigned)(next * b.w * 4) != (unsigned)(shown * b.w * 4)) {
			draw_bar(back, scanline, &b, (float)next);
			for (unsigned y = b.y - 1; y <= b.y + b.h; y++)
				memcpy(fb + (size_t)y * scanline + (size_t)(b.x - 1) * 4,
				       back + (size_t)y * scanline + (size_t)(b.x - 1) * 4,
				       (size_t)(b.w + 2) * 4);
		}
		if (next != shown)
			write_floor(next);
		shown = next;

		if (!in_initrd && shown >= 1.0) {
			if (!full_since)
				full_since = time(NULL);
			else if (time(NULL) - full_since > LINGER_SEC)
				break;
		}
		sleep_frame();
	}

	/* Stopped by GDM in the real root: finish the bar quickly (~0.2 s) so
	 * the handoff shows a complete bar instead of a stalled one. */
	if (!in_initrd && have_bar && shown < 1.0) {
		for (int i = 1; i <= 12; i++) {
			double v = shown + (1.0 - shown) * i / 12.0;
			draw_bar(back, scanline, &b, (float)v);
			for (unsigned y = b.y - 1; y <= b.y + b.h; y++)
				memcpy(fb + (size_t)y * scanline + (size_t)(b.x - 1) * 4,
				       back + (size_t)y * scanline + (size_t)(b.x - 1) * 4,
				       (size_t)(b.w + 2) * 4);
			sleep_frame();
		}
		shown = 1.0;
	}
	write_floor(shown);

	/* The picture stays on the screen after exit; the console stays
	 * unbound until splash-boot-screen-console.service gives it back. */
	return 0;
}
