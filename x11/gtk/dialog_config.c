/*
 * Copyright (c) 2002-2003 NONAKA Kimihiro
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "compiler.h"

#include "np2.h"
#include "pccore.h"

#include "sysmng.h"

#include "gtk/xnp2.h"
#include "gtk/gtk_menu.h"

static const char *baseclock_str[] = {
	"1.9968MHz", "2.4576MHz"
};

static const char *clockmult_str[] = {
	"1", "2", "4", "5", "6", "8", "10", "12", "16", "20"
};

static const struct {
	const char*	label;
	const int	rate;
} samplingrate[] = {
	{ "11KHz", 11025 },
	{ "22KHz", 22050 },
	{ "44KHz", 44100 }
};

static GtkWidget *baseclock_entry;
static GtkWidget *clockmult_entry;
static GtkWidget *buffer_entry;
static GtkWidget *resume_checkbutton;
#if defined(__GNUC__) && (defined(i386) || defined(__i386__))
static GtkWidget *disablemmx_checkbutton;
#endif
static int rate;


static void
ok_button_clicked(GtkButton *b, gpointer d)
{
	gchar *bufp = gtk_entry_get_text(GTK_ENTRY(buffer_entry));
	gchar *base = gtk_entry_get_text(GTK_ENTRY(baseclock_entry));
	gchar *multp = gtk_entry_get_text(GTK_ENTRY(clockmult_entry));
	gint resume = GTK_TOGGLE_BUTTON(resume_checkbutton)->active;
	gint disablemmx = GTK_TOGGLE_BUTTON(disablemmx_checkbutton)->active;
	guint bufsize;
	guint mult;
	UINT renewal = 0;

	UNUSED(b);

	if (strcmp(base, "1.9968MHz") == 0) {
		if (np2cfg.baseclock != PCBASECLOCK20) {
			np2cfg.baseclock = PCBASECLOCK20;
			renewal |= SYS_UPDATECFG|SYS_UPDATECLOCK;
		}
	} else {
		if (np2cfg.baseclock != PCBASECLOCK25) {
			np2cfg.baseclock = PCBASECLOCK25;
			renewal |= SYS_UPDATECFG|SYS_UPDATECLOCK;
		}
	}

	mult = milstr_solveINT(multp);
	switch (mult) {
	case 1: case 2: case 4: case 5: case 6: case 8: case 10: case 12:
	case 16: case 20:
		if (mult != np2cfg.multiple) {
			np2cfg.multiple = mult;
			renewal |= SYS_UPDATECFG|SYS_UPDATECLOCK;
		}
		break;
	}

	switch (rate) {
	case 11025:
	case 22050:
	case 44100:
		if (rate != np2cfg.samplingrate) {
			np2cfg.samplingrate = rate;
			renewal |= SYS_UPDATECFG|SYS_UPDATERATE;
			soundrenewal = 1;
		}
		break;
	}

	bufsize = milstr_solveINT(bufp);
	if (bufsize < 20)
		bufsize = 20;
	else if (bufsize > 1000)
		bufsize = 1000;
	if (np2cfg.delayms != bufsize) {
		np2cfg.delayms = bufsize;
		renewal |= SYS_UPDATECFG|SYS_UPDATESBUF;
		soundrenewal = 1;
	}

#if defined(__GNUC__) && (defined(i386) || defined(__i386__))
	if (!(mmxflag & MMXFLAG_NOTSUPPORT)) {
		disablemmx = disablemmx ? MMXFLAG_DISABLE : 0;
		if (np2oscfg.disablemmx != disablemmx) {
			np2oscfg.disablemmx = disablemmx;
			mmxflag &= ~MMXFLAG_DISABLE;
			mmxflag |= disablemmx;
			renewal |= SYS_UPDATEOSCFG;
		}
	}
#endif

	if (np2oscfg.resume != resume) {
		np2oscfg.resume = resume;
		renewal |= SYS_UPDATEOSCFG;
	}

	if (renewal) {
		sysmng_update(renewal);
	}

	gtk_widget_destroy((GtkWidget *)d);
}

static void
dialog_destroy(GtkWidget *w, GtkWidget **wp)
{

	UNUSED(wp);

	install_idle_process();
	gtk_widget_destroy(w);
}

static void
rate_radiobutton_clicked(GtkButton *b, gpointer d)
{

	UNUSED(b);

	rate = (gint)d;
}

static void
clock_changed(GtkEditable *e, gpointer d)
{
	gchar *base = gtk_entry_get_text(GTK_ENTRY(baseclock_entry));
	gchar *multp = gtk_entry_get_text(GTK_ENTRY(clockmult_entry));
	guint mult = milstr_solveINT(multp);
	gchar buf[80];
	gint clock;

	UNUSED(e);

	if (base[0] == '1') {
		clock = PCBASECLOCK20 * mult;
	} else {
		clock = PCBASECLOCK25 * mult;
	}
	g_snprintf(buf, sizeof(buf), "%2d.%4dMHz",
	    clock / 1000000U, (clock / 1000) % 1000);
	gtk_label_set_text(GTK_LABEL((GtkWidget*)d), buf);
}

void
create_configure_dialog(void)
{
	GtkWidget *config_dialog;
	GtkWidget *main_widget;
	GtkWidget *cpu_hbox;
	GtkWidget *cpu_frame;
	GtkWidget *cpuframe_vbox;
	GtkWidget *cpuclock_hbox;
	GtkWidget *baseclock_combo;
	GtkWidget *rate_combo;
	GtkWidget *times_label;
	GtkWidget *realclock_label;
	GtkWidget *confirm_widget;
	GtkWidget *ok_button;
	GtkWidget *cancel_button;
	GtkWidget *sound_frame;
	GtkWidget *soundframe_vbox;
	GtkWidget *soundrate_hbox;
	GtkWidget *rate_label;
	GtkWidget *rate_radiobutton[NELEMENTS(samplingrate)];
	GtkWidget *soundbuffer_hbox;
	GtkWidget *buffer_label;
	GtkWidget *ms_label;
	GList *baseclock_combo_items = NULL;
	GList *rate_combo_items = NULL;
	GSList *rate_group = NULL;
	gchar buf[8];
	int i;

	uninstall_idle_process();

	config_dialog = gtk_window_new(GTK_WINDOW_DIALOG);
	gtk_window_set_title(GTK_WINDOW(config_dialog), "Configure");
	gtk_window_set_position(GTK_WINDOW(config_dialog), GTK_WIN_POS_CENTER);
	gtk_window_set_modal(GTK_WINDOW(config_dialog), TRUE);
	gtk_window_set_policy(GTK_WINDOW(config_dialog), TRUE, TRUE, TRUE);
	gtk_container_set_border_width(GTK_CONTAINER(config_dialog), 5);

	gtk_signal_connect(GTK_OBJECT(config_dialog), "destroy",
	    GTK_SIGNAL_FUNC(dialog_destroy), NULL);

	main_widget = gtk_vbox_new(FALSE, 2);
	gtk_widget_show(main_widget);
	gtk_container_add(GTK_CONTAINER(config_dialog), main_widget);

	/* CPU column */
	cpu_hbox = gtk_hbox_new(FALSE, 2);
	gtk_widget_show(cpu_hbox);
	gtk_box_pack_start(GTK_BOX(main_widget), cpu_hbox, TRUE, TRUE, 0);

	/*
	 * CPU frame
	 */
	cpu_frame = gtk_frame_new("CPU");
	gtk_widget_show(cpu_frame);
	gtk_box_pack_start(GTK_BOX(cpu_hbox), cpu_frame, TRUE, TRUE, 0);

	cpuframe_vbox = gtk_vbox_new(FALSE, 2);
	gtk_container_set_border_width(GTK_CONTAINER(cpuframe_vbox), 5);
	gtk_widget_show(cpuframe_vbox);
	gtk_container_add(GTK_CONTAINER(cpu_frame), cpuframe_vbox);

	/* cpu clock */
	cpuclock_hbox = gtk_hbox_new(FALSE, 3);
	gtk_widget_show(cpuclock_hbox);
	gtk_box_pack_start(GTK_BOX(cpuframe_vbox),cpuclock_hbox, TRUE, TRUE, 2);

	baseclock_combo = gtk_combo_new();
	gtk_widget_show(baseclock_combo);
	gtk_box_pack_start(GTK_BOX(cpuclock_hbox), baseclock_combo, TRUE, FALSE, 0);
	gtk_widget_set_usize(baseclock_combo, 96, 0);
	gtk_combo_set_value_in_list(GTK_COMBO(baseclock_combo), TRUE, TRUE);
	gtk_combo_set_use_arrows_always(GTK_COMBO(baseclock_combo), TRUE);
	for (i = 0; i < NELEMENTS(baseclock_str); i++) {
		baseclock_combo_items = g_list_append(baseclock_combo_items, (gpointer)baseclock_str[i]);
	}
	gtk_combo_set_popdown_strings(GTK_COMBO(baseclock_combo),
	    baseclock_combo_items);
	g_list_free(baseclock_combo_items);

	baseclock_entry = GTK_COMBO(baseclock_combo)->entry;
	gtk_widget_show(baseclock_entry);
	gtk_entry_set_editable(GTK_ENTRY(baseclock_entry), FALSE);
	switch (np2cfg.baseclock) {
	default:
		np2cfg.baseclock = PCBASECLOCK25;
		sysmng_update(SYS_UPDATECFG|SYS_UPDATECLOCK);
		/*FALLTHROUGH*/
	case PCBASECLOCK25:
		gtk_entry_set_text(GTK_ENTRY(baseclock_entry),baseclock_str[1]);
		break;

	case PCBASECLOCK20:
		gtk_entry_set_text(GTK_ENTRY(baseclock_entry),baseclock_str[0]);
		break;
	}

	times_label = gtk_label_new("x");
	gtk_widget_show(times_label);
	gtk_box_pack_start(GTK_BOX(cpuclock_hbox), times_label, TRUE, FALSE, 0);
	gtk_misc_set_padding(GTK_MISC(times_label), 5, 0);

	rate_combo = gtk_combo_new();
	gtk_widget_show(rate_combo);
	gtk_box_pack_start(GTK_BOX(cpuclock_hbox), rate_combo, TRUE, FALSE, 0);
	gtk_widget_set_usize(rate_combo, 48, 0);
	gtk_combo_set_value_in_list(GTK_COMBO(rate_combo), TRUE, TRUE);
	gtk_combo_set_use_arrows_always(GTK_COMBO(rate_combo), TRUE);
	for (i = 0; i < NELEMENTS(clockmult_str); i++) {
		rate_combo_items = g_list_append(rate_combo_items, (gpointer)clockmult_str[i]);
	}
	gtk_combo_set_popdown_strings(GTK_COMBO(rate_combo), rate_combo_items);
	g_list_free(rate_combo_items);

	clockmult_entry = GTK_COMBO(rate_combo)->entry;
	gtk_widget_show(clockmult_entry);
	gtk_entry_set_editable(GTK_ENTRY(clockmult_entry), FALSE);
	switch (np2cfg.multiple) {
	case 1: case 2: case 4: case 5: case 6: case 8: case 10: case 12:
	case 16: case 20:
		g_snprintf(buf, sizeof(buf), "%d", np2cfg.multiple);
		gtk_entry_set_text(GTK_ENTRY(clockmult_entry), buf);
		break;

	default:
		gtk_entry_set_text(GTK_ENTRY(clockmult_entry), "4");
		break;
	}

	/* calculated cpu clock */
	realclock_label = gtk_label_new("MHz");
	gtk_widget_show(realclock_label);
	gtk_box_pack_start(GTK_BOX(cpuframe_vbox), realclock_label, FALSE, FALSE, 2);
	gtk_misc_set_alignment(GTK_MISC(realclock_label), 1.0, 0.5);

	gtk_signal_connect(GTK_OBJECT(baseclock_entry), "changed",
	  GTK_SIGNAL_FUNC(clock_changed), (gpointer)realclock_label);
	gtk_signal_connect(GTK_OBJECT(clockmult_entry), "changed",
	  GTK_SIGNAL_FUNC(clock_changed), (gpointer)realclock_label);
	clock_changed(NULL, realclock_label);

	/* OK, Cancel button base widget */
	confirm_widget = gtk_vbutton_box_new();
	gtk_widget_show(confirm_widget);
	gtk_box_pack_start(GTK_BOX(cpu_hbox), confirm_widget, TRUE, TRUE, 0);
	gtk_button_box_set_layout(GTK_BUTTON_BOX(confirm_widget), GTK_BUTTONBOX_END);
	gtk_button_box_set_spacing(GTK_BUTTON_BOX(confirm_widget), 0);

	/*
	 * Sound frame
	 */
	sound_frame = gtk_frame_new("Sound");
	gtk_widget_show(sound_frame);
	gtk_box_pack_start(GTK_BOX(main_widget), sound_frame, TRUE, TRUE, 0);

	soundframe_vbox = gtk_vbox_new(FALSE, 0);
	gtk_container_set_border_width(GTK_CONTAINER(soundframe_vbox), 5);
	gtk_widget_show(soundframe_vbox);
	gtk_container_add(GTK_CONTAINER(sound_frame), soundframe_vbox);

	/* sampling rate */
	soundrate_hbox = gtk_hbox_new(FALSE, 2);
	gtk_widget_show(soundrate_hbox);
	gtk_box_pack_start(GTK_BOX(soundframe_vbox), soundrate_hbox, TRUE, TRUE, 2);

	rate_label = gtk_label_new("Sampling Rate");
	gtk_widget_show(rate_label);
	gtk_box_pack_start(GTK_BOX(soundrate_hbox), rate_label, FALSE, TRUE, 3);
	gtk_widget_set_usize(rate_label, 96, 0);

	for (i = 0; i < NELEMENTS(samplingrate); i++) {
		rate_radiobutton[i] = gtk_radio_button_new_with_label(rate_group, samplingrate[i].label);
		rate_group = gtk_radio_button_group(GTK_RADIO_BUTTON(rate_radiobutton[i]));
		gtk_widget_show(rate_radiobutton[i]);
		gtk_box_pack_start(GTK_BOX(soundrate_hbox), rate_radiobutton[i], FALSE, FALSE, 0);
		GTK_WIDGET_UNSET_FLAGS(rate_radiobutton[i], GTK_CAN_FOCUS);
		gtk_signal_connect(GTK_OBJECT(rate_radiobutton[i]), "clicked",
		    GTK_SIGNAL_FUNC(rate_radiobutton_clicked), (gpointer)samplingrate[i].rate);
	}
	if (np2cfg.samplingrate == 11025) {
		i = 0;
	} else if (np2cfg.samplingrate == 22050) {
		i = 1;
	} else if (np2cfg.samplingrate == 44100) {
		i = 2;
	} else {
		i = 1;
		np2cfg.samplingrate = 22050;
		sysmng_update(SYS_UPDATECFG|SYS_UPDATERATE);
	}
	gtk_signal_emit_by_name(GTK_OBJECT(rate_radiobutton[i]), "clicked");

	soundbuffer_hbox = gtk_hbox_new(FALSE, 2);
	gtk_widget_show(soundbuffer_hbox);
	gtk_box_pack_start(GTK_BOX(soundframe_vbox), soundbuffer_hbox, TRUE, TRUE, 2);

	/* buffer size */
	buffer_label = gtk_label_new("Buffer");
	gtk_widget_show(buffer_label);
	gtk_box_pack_start(GTK_BOX(soundbuffer_hbox), buffer_label, FALSE, FALSE, 0);
	gtk_widget_set_usize(buffer_label, 96, 0);

	buffer_entry = gtk_entry_new();
	gtk_widget_show(buffer_entry);
	gtk_box_pack_start(GTK_BOX(soundbuffer_hbox), buffer_entry, FALSE, FALSE, 0);
	gtk_widget_set_usize(buffer_entry, 48, 0);

	if (np2cfg.delayms >= 20 && np2cfg.delayms <= 1000) {
		g_snprintf(buf, sizeof(buf), "%d", np2cfg.delayms);
		gtk_entry_set_text(GTK_ENTRY(buffer_entry), buf);
	} else {
		gtk_entry_set_text(GTK_ENTRY(buffer_entry), "800");
		np2cfg.delayms = 800;
		sysmng_update(SYS_UPDATECFG|SYS_UPDATESBUF);
		soundrenewal = 1;
	}

	ms_label = gtk_label_new(" ms");
	gtk_widget_show(ms_label);
	gtk_box_pack_start(GTK_BOX(soundbuffer_hbox),ms_label, FALSE, FALSE, 0);

	/* resume */
	resume_checkbutton = gtk_check_button_new_with_label("Resume");
	gtk_widget_show(resume_checkbutton);
	gtk_box_pack_start(GTK_BOX(main_widget), resume_checkbutton, FALSE, FALSE, 1);
	if (np2oscfg.resume) {
		gtk_signal_emit_by_name(GTK_OBJECT(resume_checkbutton), "clicked");
	}

#if defined(__GNUC__) && (defined(i386) || defined(__i386__))
	/* Disable MMX */
	disablemmx_checkbutton = gtk_check_button_new_with_label("Disable MMX");
	gtk_widget_show(disablemmx_checkbutton);
	gtk_box_pack_start(GTK_BOX(main_widget), disablemmx_checkbutton, FALSE, FALSE, 1);
	if (mmxflag & MMXFLAG_NOTSUPPORT) {
		gtk_widget_set_sensitive(disablemmx_checkbutton, FALSE);
	} else if (mmxflag & MMXFLAG_DISABLE) {
		gtk_signal_emit_by_name(GTK_OBJECT(disablemmx_checkbutton), "clicked");
	}
#endif

	/*
	 * OK, Cancel button
	 */
	ok_button = gtk_button_new_with_label("OK");
	gtk_widget_set_usize(ok_button, 80, 0);
	gtk_widget_show(ok_button);
	gtk_container_add(GTK_CONTAINER(confirm_widget), ok_button);
	GTK_WIDGET_SET_FLAGS(ok_button, GTK_CAN_DEFAULT);
	GTK_WIDGET_SET_FLAGS(ok_button, GTK_HAS_DEFAULT);
	gtk_signal_connect(GTK_OBJECT(ok_button), "clicked",
	    GTK_SIGNAL_FUNC(ok_button_clicked), (gpointer)config_dialog);
	gtk_widget_grab_default(ok_button);

	cancel_button = gtk_button_new_with_label("Cancel");
	gtk_widget_set_usize(cancel_button, 80, 0);
	gtk_widget_show(cancel_button);
	gtk_container_add(GTK_CONTAINER(confirm_widget), cancel_button);
	GTK_WIDGET_SET_FLAGS(cancel_button, GTK_CAN_DEFAULT);
	gtk_signal_connect_object(GTK_OBJECT(cancel_button), "clicked",
	    GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(config_dialog));

	gtk_widget_show_all(config_dialog);
}
