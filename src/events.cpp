/*
* UAE - The Un*x Amiga Emulator
*
* Event stuff
*
* Copyright 1995-2002 Bernd Schmidt
* Copyright 1995 Alessandro Bissacco
* Copyright 2000-2012 Toni Wilen
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "custom.h"
#include "events.h"
#include "memory.h"
#include "newcpu.h"
#include "uae/ppc.h"
#include "xwin.h"
#include "x86.h"
#include "audio.h"

static const int pissoff_nojit_value = 256 * CYCLE_UNIT;

unsigned long int event_cycles, nextevent, currcycle;
int is_syncline, is_syncline_end;
long cycles_to_next_event;
long max_cycles_to_next_event;
long cycles_to_hsync_event;
unsigned long start_cycles;
bool event_wait;

frame_time_t vsyncmintime, vsyncmintimepre;
frame_time_t vsyncmaxtime, vsyncwaittime;
int vsynctimebase;
int event2_count;

static void events_fast(void)
{
	cycles_do_special();
}

void events_reset_syncline(void)
{
	is_syncline = 0;
	events_fast();
}

void events_schedule (void)
{
	int i;

	unsigned long int mintime = ~0L;
	for (i = 0; i < ev_max; i++) {
		if (eventtab[i].active) {
			unsigned long int eventtime = eventtab[i].evtime - currcycle;
			if (eventtime < mintime)
				mintime = eventtime;
		}
	}
	nextevent = currcycle + mintime;
}

extern int vsync_activeheight;

#ifdef FSUAE
#include "fsemu-frame.h"
#include "fsemu-time.h"
extern int64_t is_syncline_end64;
extern int64_t line_started_at;
extern int64_t line_ended_at;
#endif

static bool event_check_vsync(void)
{
#ifdef FSUAE
	// uae_log("event_check_vsync is_syncline %d event_wait %d\n", is_syncline, event_wait);
#endif
	/* Keep only CPU emulation running while waiting for sync point. */
	if (is_syncline == -1) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		// wait for vblank
		audio_finish_pull();
		int done = vsync_isdone(NULL);
		if (done == -2) {
			// if no vsync thread
			int vp = target_get_display_scanline(-1);
			if (vp < is_syncline_end)
				done = 1;
			else if (vp > is_syncline_end)
				is_syncline_end = vp;
		}
		if (!done) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_quick();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_clear();
		vsync_event_done();

	} else if (is_syncline == -2) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		// wait for vblank or early vblank
		audio_finish_pull();
		int done = vsync_isdone(NULL);
		if (done == -2)
			done = 0;
		int vp = target_get_display_scanline(-1);
		if (vp < 0 || vp >= is_syncline_end)
			done = 1;
		if (!done) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_quick();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_clear();
		vsync_event_done();

	} else if (is_syncline == -3) {
		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		// not vblank
		audio_finish_pull();
		int vp = target_get_display_scanline(-1);
		if (vp <= 0) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_quick();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_clear();
		vsync_event_done();

	} else if (is_syncline > 0) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		audio_finish_pull();
		// wait for specific scanline
		int vp = target_get_display_scanline(-1);
		if (vp < 0 || is_syncline > vp) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_check();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_event_done();

	}
	else if (is_syncline <= -100) {

		if (!isvsync_chipset()) {
			events_reset_syncline();
			return false;
		}
		audio_finish_pull();
		// wait for specific scanline
		int vp = target_get_display_scanline(-1);
		if (vp < 0 || vp >= (-(is_syncline + 100))) {
#ifdef WITH_PPC
			if (ppc_state) {
				uae_ppc_execute_check();
			}
#endif
			if (currprefs.cachesize)
				pissoff = pissoff_value;
			else
				pissoff = pissoff_nojit_value;
			return true;
		}
		vsync_event_done();

	} else if (is_syncline == -10) {

		// wait is_syncline_end
		if (event_wait) {
			int rpt = read_processor_time();
			int v = rpt - is_syncline_end;
			if (v < 0) {
#ifdef WITH_PPC
				if (ppc_state) {
					uae_ppc_execute_check();
				}
#endif
				if (currprefs.cachesize)
					pissoff = pissoff_value;
				else
					pissoff = pissoff_nojit_value;
				return true;
			}
		}
		events_reset_syncline();

#ifdef FSUAE // NL
	} else if (is_syncline == -99) {
		// int64_t now = fsemu_time_us();

		if (event_wait) {
			// int64_t v = now - is_syncline_end64;
			int64_t v = fsemu_time_us() - is_syncline_end64;
			// uae_log("%lld\n", (long long) v);			
			if (v < 0) {
#ifdef WITH_PPC
				if (ppc_state) {
					uae_ppc_execute_check();
				}
#endif
				if (currprefs.cachesize)
					pissoff = pissoff_value;
				else
					pissoff = pissoff_nojit_value;
				
				// fsemu_frame_extra_duration += now - line_ended_at;
				// line_started_at = now;
				return true;
			}
		}

		// int64_t now = fsemu_time_us();
		// fsemu_frame_extra_duration += now - line_ended_at;
		// line_started_at = now;

		// uae_log("reset_syncline\n");
		// events_reset_syncline();

		// vsync_event_done calls events_reset_syncline
		vsync_event_done();
#endif

	} else if (is_syncline < -10) {

		// wait is_syncline_end/vsyncmintime
		if (event_wait) {
			int rpt = read_processor_time();
			int v = rpt - vsyncmintime;
			int v2 = rpt - is_syncline_end;
			if (v > vsynctimebase || v < -vsynctimebase) {
				v = 0;
			}
			if (v < 0 && v2 < 0) {
#ifdef WITH_PPC
				if (ppc_state) {
					if (is_syncline == -11) {
						uae_ppc_execute_check();
					} else {
						uae_ppc_execute_quick();
					}
				}
#endif
				if (currprefs.cachesize)
					pissoff = pissoff_value;
				else
					pissoff = pissoff_nojit_value;
				return true;
			}
		}
		events_reset_syncline();
	}
	return false;
}

void do_cycles_slow (unsigned long cycles_to_add)
{
#ifdef WITH_X86
#if 0
	if (x86_turbo_on) {
		execute_other_cpu_single();
	}
#endif
#endif

	if (!currprefs.cpu_thread) {
		if ((pissoff -= cycles_to_add) >= 0)
			return;

		cycles_to_add = -pissoff;
		pissoff = 0;
	} else {
		pissoff = 0x40000000;
	}

	while ((nextevent - currcycle) <= cycles_to_add) {

		if (is_syncline) {
			if (event_check_vsync())
				return;
		}

		cycles_to_add -= nextevent - currcycle;
		currcycle = nextevent;

		for (int i = 0; i < ev_max; i++) {
			if (eventtab[i].active && eventtab[i].evtime == currcycle) {
				if (eventtab[i].handler == NULL) {
					gui_message(_T("eventtab[%d].handler is null!\n"), i);
					eventtab[i].active = 0;
				} else {
					(*eventtab[i].handler)();
				}
			}
		}
		events_schedule ();


	}
	currcycle += cycles_to_add;
}

void MISC_handler (void)
{
	static bool dorecheck;
	bool recheck;
	int i;
	evt mintime;
	evt ct = get_cycles ();
	static int recursive;

	if (recursive) {
		dorecheck = true;
		return;
	}
	recursive++;
	eventtab[ev_misc].active = 0;
	recheck = true;
	while (recheck) {
		recheck = false;
		mintime = ~0L;
		for (i = 0; i < ev2_max; i++) {
			if (eventtab2[i].active) {
				if (eventtab2[i].evtime == ct) {
					eventtab2[i].active = false;
					event2_count--;
					eventtab2[i].handler (eventtab2[i].data);
					if (dorecheck || eventtab2[i].active) {
						recheck = true;
						dorecheck = false;
					}
				} else {
					evt eventtime = eventtab2[i].evtime - ct;
					if (eventtime < mintime)
						mintime = eventtime;
				}
			}
		}
	}
	if (mintime != ~0UL) {
		eventtab[ev_misc].active = true;
		eventtab[ev_misc].oldcycles = ct;
		eventtab[ev_misc].evtime = ct + mintime;
		events_schedule ();
	}
	recursive--;
}

#ifdef FSUAE_RECORDING

#include "savestate.h"

void uae_events_trace_log(void)
{
	for (int i = 0; i < ev_max; i++) {
		printf("eventtab[%d] %d %lu %lu\n", i, eventtab[i].active, eventtab[i].evtime, eventtab[i].oldcycles);
	}
	for (int i = 0; i < ev2_max; i++) {
		printf("eventtab2[%d] %d %lu %08x\n", i, eventtab2[i].active, eventtab2[i].evtime, eventtab2[i].data);
	}
}

static int next_event_no = ev2_misc;
#define next next_event_no
#endif

void event2_newevent_xx (int no, evt t, uae_u32 data, evfunc2 func)
{
	evt et;
#ifdef FSUAE_RECORDING
	// Moved to global scope so we can save/restore this value, to ensure that
	// events are performed in the right order.
#else
	static int next = ev2_misc;
#endif

	et = t + get_cycles ();
	if (no < 0) {
		no = next;
		for (;;) {
			if (!eventtab2[no].active) {
				event2_count++;
				break;
			}
			if (eventtab2[no].evtime == et && eventtab2[no].handler == func && eventtab2[no].data == data)
				break;
			no++;
			if (no == ev2_max)
				no = ev2_misc;
			if (no == next) {
				write_log (_T("out of event2's!\n"));
				return;
			}
		}
		next = no;
	}
	eventtab2[no].active = true;
	eventtab2[no].evtime = et;
	eventtab2[no].handler = func;
	eventtab2[no].data = data;
	MISC_handler ();
}

void event2_newevent_x_replace(evt t, uae_u32 data, evfunc2 func)
{
	for (int i = 0; i < ev2_max; i++) {
		if (eventtab2[i].active && eventtab2[i].handler == func) {
			eventtab2[i].active = false;
		}
	}
	if (((int)t) <= 0) {
		func(data);
		return;
	}
	event2_newevent_xx(-1, t * CYCLE_UNIT, data, func);
}

#ifdef FSUAE_RECORDING

#include <savestate.h>
#include <cia.h>
#include <custom.h>

extern void hsync_handler (void);

static uint8_t handler_to_int(evfunc handler)
{
	if (handler == NULL) {
		return 0;
	}
	if (handler == CIA_handler) {
		return 1;
	}
	if (handler == hsync_handler) {
		return 2;
	}
	if (handler == MISC_handler) {
		return 3;
	}
	if (handler == audio_evhandler) {
		return 4;
	}
	printf("WARNING: Unrecognized handler when saving\n");
	return 0xFF;
}

static evfunc int_to_handler(uint8_t handler)
{
	if (handler == 0) {
		return NULL;
	}
	if (handler == 1) {
		return CIA_handler;
	}
	if (handler == 2) {
		return hsync_handler;
	}
	if (handler == 3) {
		return MISC_handler;
	}
	if (handler == 4) {
		return audio_evhandler;
	}
	printf("WARNING: Unrecognized handler when restoring\n");
	return NULL;
}

static uint8_t handler2_to_int(evfunc2 handler)
{
	if (handler == NULL) {
		return 0;
	}
	if (handler == action_replay_cia_access_delay) {
		return 1;
	}
	if (handler == audio_setirq_event) {
		return 2;
	}
	if (handler == ICRA) {
		return 3;
	}
	if (handler == ICRB) {
		return 4;
	}
	if (handler == CIAB_tod_inc_event) {
		return 5;
	}
	if (handler == CIAA_tod_handler) {
		return 6;
	}
	if (handler == send_interrupt_do) {
		return 7;
	}
	if (handler == send_intena_do) {
		return 8;
	}
	if (handler == send_intreq_do) {
		return 9;
	}
	if (handler == lightpen_trigger_func) {
		return 10;
	}
	if (handler == breakfunc) {
		return 11;
	}
	if (handler == blitter_handler) {
		return 12;
	}
	if (handler == DISK_handler) {
		return 13;
	}
	if (handler == subcode_interrupt) {
		return 14;
	}
	if (handler == copper_write) {
		return 15;
	}
	if (handler == dmal_func) {
		return 16;
	}
	if (handler == dmal_func2) {
		return 17;
	}
	if (handler == motordelay_func) {
		return 18;
	}

// subcode_interrupt
// copper_write
// dmal_func
// dmal_func2
// motordelay_func
	// if (handler == sersend_c) {
	// 	return ;
	// }

	printf("WARNING: Unrecognized handler2 (%p) when saving\n", handler);
	return 0xFF;
}

// FIXME
// evtfunc (ahi_v2)
// evtfunc (ahidsound_new2)
// evtfunc (ahidsoundx_new)
// sersend_ce

static evfunc2 int_to_handler2(uint8_t handler)
{
	if (handler == 0) {
		return NULL;
	}
	if (handler == 1) {
		return action_replay_cia_access_delay;
	}
	if (handler == 2) {
		return audio_setirq_event;
	}
	if (handler == 3) {
		return ICRA;
	}
	if (handler == 4) {
		return ICRB;
	}
	if (handler == 5) {
		return CIAB_tod_inc_event;
	}
	if (handler == 6) {
		return CIAA_tod_handler;
	}
	if (handler == 7) {
		return send_interrupt_do;
	}
	if (handler == 8) {
		return send_intena_do;
	}
	if (handler == 9) {
		return send_intreq_do;
	}
	if (handler == 10) {
		return lightpen_trigger_func;
	}
	if (handler == 11) {
		return breakfunc;
	}
	if (handler == 12) {
		return blitter_handler;
	}
	if (handler == 13) {
		return DISK_handler;
	}
	if (handler == 14) {
		return subcode_interrupt;
	}
	if (handler == 15) {
		return copper_write;
	}
	if (handler == 16) {
		return dmal_func;
	}
	if (handler == 17) {
		return dmal_func2;
	}
	if (handler == 18) {
		return motordelay_func;
	}
	// if (handler == sersend_c) {
	// 	return ;
	// }
	printf("WARNING: Unrecognized handler2 when restoring\n");
	return NULL;
}

void uae_events_save_state_fs(uae_savestate_context_t *ctx)
{
	char name[32 + 1];

	// int is_syncline, is_syncline_end;
	// bool event_wait;

	uae_savestate_ulong(ctx, "event_cycles", &event_cycles);
	uae_savestate_ulong(ctx, "nextevent", &nextevent);
	uae_savestate_ulong(ctx, "currcycle", &currcycle);
	uae_savestate_long(ctx, "cycles_to_next_event", &cycles_to_next_event);
	uae_savestate_long(ctx, "max_cycles_to_next_event", &max_cycles_to_next_event);
	uae_savestate_long(ctx, "cycles_to_hsync_event", &cycles_to_hsync_event);
	uae_savestate_ulong(ctx, "start_cycles", &start_cycles);

	uae_savestate_int(ctx, "next_event_no", &next_event_no);

	for (int i = 0; i < ev_max; i++) {
		sprintf(name, "eventtab[%d].active", i);
		uae_savestate_bool(ctx, name, &eventtab[i].active);
		sprintf(name, "eventtab[%d].evtime", i);
		uae_savestate_ulong(ctx, name, &eventtab[i].evtime);
		sprintf(name, "eventtab[%d].oldcycles", i);
		uae_savestate_ulong(ctx, name, &eventtab[i].oldcycles);
		sprintf(name, "eventtab[%d].handler", i);
		uint8_t handler = handler_to_int(eventtab[i].handler);
		uae_savestate_uint8(ctx, name, &handler);
		if (ctx->load) {
			eventtab[i].handler = int_to_handler(handler);
		}
	}

	for (int i = 0; i < ev2_max; i++) {
		sprintf(name, "eventtab2[%d].active", i);
		uae_savestate_bool(ctx, name, &eventtab2[i].active);
		sprintf(name, "eventtab2[%d].evtime", i);
		uae_savestate_ulong(ctx, name, &eventtab2[i].evtime);
		sprintf(name, "eventtab2[%d].handler", i);
		uint8_t handler = handler2_to_int(eventtab2[i].handler);
		uae_savestate_uint8(ctx, name, &handler);
		if (ctx->load) {
			eventtab2[i].handler = int_to_handler2(handler);
		}
		sprintf(name, "eventtab2[%d].data", i);
		uae_savestate_uint32(ctx, name, &eventtab2[i].data);
	}

    bool active;
    evt evtime;
    uae_u32 data;
    evfunc2 handler;

	struct ev eventtab[ev_max];
	struct ev2 eventtab2[ev2_max];

}

#endif  // FSUAE_RECORDING
