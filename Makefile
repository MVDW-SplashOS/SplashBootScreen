PREFIX     ?= /usr
UNITDIR    ?= $(PREFIX)/lib/systemd/system
DRACUTDIR  ?= $(PREFIX)/lib/dracut/modules.d
CFLAGS     ?= -O2
CFLAGS     += -Wall -Wextra
LDLIBS     += -lsystemd -lm

all: splash-boot-screen boot-logo-static.raw

splash-boot-screen: splash-boot-screen.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

boot-logo-static.raw: assets/boot-logo-static.raw.xz
	xz -dc $< > $@

install: all
	install -Dm755 splash-boot-screen $(DESTDIR)$(PREFIX)/bin/splash-boot-screen
	install -Dm644 boot-logo-static.raw $(DESTDIR)$(PREFIX)/share/splashos/boot-logo-static.raw
	install -Dm644 systemd/splash-boot-screen.service $(DESTDIR)$(UNITDIR)/splash-boot-screen.service
	install -Dm644 systemd/splash-boot-screen-console.service $(DESTDIR)$(UNITDIR)/splash-boot-screen-console.service
	for d in gdm emergency rescue; do \
		install -Dm644 systemd/$$d.service.d/50-splash-boot-screen.conf \
			$(DESTDIR)$(UNITDIR)/$$d.service.d/50-splash-boot-screen.conf; \
	done
	install -Dm755 dracut/90splash-boot-screen/module-setup.sh \
		$(DESTDIR)$(DRACUTDIR)/90splash-boot-screen/module-setup.sh
	install -d $(DESTDIR)$(UNITDIR)/sysinit.target.wants $(DESTDIR)$(UNITDIR)/graphical.target.wants
	ln -sf ../splash-boot-screen.service $(DESTDIR)$(UNITDIR)/sysinit.target.wants/splash-boot-screen.service
	ln -sf ../splash-boot-screen-console.service $(DESTDIR)$(UNITDIR)/graphical.target.wants/splash-boot-screen-console.service

clean:
	rm -f splash-boot-screen boot-logo-static.raw

.PHONY: all install clean
