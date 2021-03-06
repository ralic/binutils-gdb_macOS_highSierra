/* Serial interface for local (hardwired) serial ports on Un*x like systems

   Copyright (C) 1992-2017 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "serial.h"
#include "ser-base.h"
#include "ser-unix.h"

#include <fcntl.h>
#include <sys/types.h>
#include "terminal.h"
#include <sys/socket.h>
#include "gdb_sys_time.h"

#include "gdb_select.h"
#include "gdbcmd.h"
#include "filestuff.h"
#include "gdb_termios.h"

#ifdef HAVE_TERMIOS

struct hardwire_ttystate
  {
    struct termios termios;
  };

#ifdef CRTSCTS
/* Boolean to explicitly enable or disable h/w flow control.  */
static int serial_hwflow;
static void
show_serial_hwflow (struct ui_file *file, int from_tty,
		    struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("Hardware flow control is %s.\n"), value);
}
#endif

#endif /* termios */

#ifdef HAVE_TERMIO

/* It is believed that all systems which have added job control to SVR3
   (e.g. sco) have also added termios.  Even if not, trying to figure out
   all the variations (TIOCGPGRP vs. TCGETPGRP, etc.) would be pretty
   bewildering.  So we don't attempt it.  */

struct hardwire_ttystate
  {
    struct termio termio;
  };
#endif /* termio */

#ifdef HAVE_SGTTY
struct hardwire_ttystate
  {
    struct sgttyb sgttyb;
    struct tchars tc;
    struct ltchars ltc;
    /* Line discipline flags.  */
    int lmode;
  };
#endif /* sgtty */

static int hardwire_open (struct serial *scb, const char *name);
static void hardwire_raw (struct serial *scb);
static int rate_to_code (int rate);
static int hardwire_setbaudrate (struct serial *scb, int rate);
static int hardwire_setparity (struct serial *scb, int parity);
static void hardwire_close (struct serial *scb);
static int get_tty_state (struct serial *scb,
			  struct hardwire_ttystate * state);
static int set_tty_state (struct serial *scb,
			  struct hardwire_ttystate * state);
static serial_ttystate hardwire_get_tty_state (struct serial *scb);
static int hardwire_set_tty_state (struct serial *scb, serial_ttystate state);
static int hardwire_noflush_set_tty_state (struct serial *, serial_ttystate,
					   serial_ttystate);
static void hardwire_print_tty_state (struct serial *, serial_ttystate,
				      struct ui_file *);
static int hardwire_drain_output (struct serial *);
static int hardwire_flush_output (struct serial *);
static int hardwire_flush_input (struct serial *);
static int hardwire_send_break (struct serial *);
static int hardwire_setstopbits (struct serial *, int);

/* Open up a real live device for serial I/O.  */

static int
hardwire_open (struct serial *scb, const char *name)
{
  scb->fd = gdb_open_cloexec (name, O_RDWR, 0);
  if (scb->fd < 0)
    return -1;

  return 0;
}

static int
get_tty_state (struct serial *scb, struct hardwire_ttystate *state)
{
#ifdef HAVE_TERMIOS
  if (tcgetattr (scb->fd, &state->termios) < 0)
    return -1;

  return 0;
#endif

#ifdef HAVE_TERMIO
  if (ioctl (scb->fd, TCGETA, &state->termio) < 0)
    return -1;
  return 0;
#endif

#ifdef HAVE_SGTTY
  if (ioctl (scb->fd, TIOCGETP, &state->sgttyb) < 0)
    return -1;
  if (ioctl (scb->fd, TIOCGETC, &state->tc) < 0)
    return -1;
  if (ioctl (scb->fd, TIOCGLTC, &state->ltc) < 0)
    return -1;
  if (ioctl (scb->fd, TIOCLGET, &state->lmode) < 0)
    return -1;

  return 0;
#endif
}

static int
set_tty_state (struct serial *scb, struct hardwire_ttystate *state)
{
#ifdef HAVE_TERMIOS
  if (tcsetattr (scb->fd, TCSANOW, &state->termios) < 0)
    return -1;

  return 0;
#endif

#ifdef HAVE_TERMIO
  if (ioctl (scb->fd, TCSETA, &state->termio) < 0)
    return -1;
  return 0;
#endif

#ifdef HAVE_SGTTY
  if (ioctl (scb->fd, TIOCSETN, &state->sgttyb) < 0)
    return -1;
  if (ioctl (scb->fd, TIOCSETC, &state->tc) < 0)
    return -1;
  if (ioctl (scb->fd, TIOCSLTC, &state->ltc) < 0)
    return -1;
  if (ioctl (scb->fd, TIOCLSET, &state->lmode) < 0)
    return -1;

  return 0;
#endif
}

static serial_ttystate
hardwire_get_tty_state (struct serial *scb)
{
  struct hardwire_ttystate *state = XNEW (struct hardwire_ttystate);

  if (get_tty_state (scb, state))
    {
      xfree (state);
      return NULL;
    }

  return (serial_ttystate) state;
}

static serial_ttystate
hardwire_copy_tty_state (struct serial *scb, serial_ttystate ttystate)
{
  struct hardwire_ttystate *state = XNEW (struct hardwire_ttystate);

  *state = *(struct hardwire_ttystate *) ttystate;

  return (serial_ttystate) state;
}

static int
hardwire_set_tty_state (struct serial *scb, serial_ttystate ttystate)
{
  struct hardwire_ttystate *state;

  state = (struct hardwire_ttystate *) ttystate;

  return set_tty_state (scb, state);
}

static int
hardwire_noflush_set_tty_state (struct serial *scb,
				serial_ttystate new_ttystate,
				serial_ttystate old_ttystate)
{
  struct hardwire_ttystate new_state;
#ifdef HAVE_SGTTY
  struct hardwire_ttystate *state = (struct hardwire_ttystate *) old_ttystate;
#endif

  new_state = *(struct hardwire_ttystate *) new_ttystate;

  /* Don't change in or out of raw mode; we don't want to flush input.
     termio and termios have no such restriction; for them flushing input
     is separate from setting the attributes.  */

#ifdef HAVE_SGTTY
  if (state->sgttyb.sg_flags & RAW)
    new_state.sgttyb.sg_flags |= RAW;
  else
    new_state.sgttyb.sg_flags &= ~RAW;

  /* I'm not sure whether this is necessary; the manpage just mentions
     RAW not CBREAK.  */
  if (state->sgttyb.sg_flags & CBREAK)
    new_state.sgttyb.sg_flags |= CBREAK;
  else
    new_state.sgttyb.sg_flags &= ~CBREAK;
#endif

  return set_tty_state (scb, &new_state);
}

static void
hardwire_print_tty_state (struct serial *scb,
			  serial_ttystate ttystate,
			  struct ui_file *stream)
{
  struct hardwire_ttystate *state = (struct hardwire_ttystate *) ttystate;
  int i;

#ifdef HAVE_TERMIOS
  fprintf_filtered (stream, "c_iflag = 0x%x, c_oflag = 0x%x,\n",
		    (int) state->termios.c_iflag,
		    (int) state->termios.c_oflag);
  fprintf_filtered (stream, "c_cflag = 0x%x, c_lflag = 0x%x\n",
		    (int) state->termios.c_cflag,
		    (int) state->termios.c_lflag);
#if 0
  /* This not in POSIX, and is not really documented by those systems
     which have it (at least not Sun).  */
  fprintf_filtered (stream, "c_line = 0x%x.\n", state->termios.c_line);
#endif
  fprintf_filtered (stream, "c_cc: ");
  for (i = 0; i < NCCS; i += 1)
    fprintf_filtered (stream, "0x%x ", state->termios.c_cc[i]);
  fprintf_filtered (stream, "\n");
#endif

#ifdef HAVE_TERMIO
  fprintf_filtered (stream, "c_iflag = 0x%x, c_oflag = 0x%x,\n",
		    state->termio.c_iflag, state->termio.c_oflag);
  fprintf_filtered (stream, "c_cflag = 0x%x, c_lflag = 0x%x, c_line = 0x%x.\n",
		    state->termio.c_cflag, state->termio.c_lflag,
		    state->termio.c_line);
  fprintf_filtered (stream, "c_cc: ");
  for (i = 0; i < NCC; i += 1)
    fprintf_filtered (stream, "0x%x ", state->termio.c_cc[i]);
  fprintf_filtered (stream, "\n");
#endif

#ifdef HAVE_SGTTY
  fprintf_filtered (stream, "sgttyb.sg_flags = 0x%x.\n",
		    state->sgttyb.sg_flags);

  fprintf_filtered (stream, "tchars: ");
  for (i = 0; i < (int) sizeof (struct tchars); i++)
    fprintf_filtered (stream, "0x%x ", ((unsigned char *) &state->tc)[i]);
  fprintf_filtered (stream, "\n");

  fprintf_filtered (stream, "ltchars: ");
  for (i = 0; i < (int) sizeof (struct ltchars); i++)
    fprintf_filtered (stream, "0x%x ", ((unsigned char *) &state->ltc)[i]);
  fprintf_filtered (stream, "\n");

  fprintf_filtered (stream, "lmode:  0x%x\n", state->lmode);
#endif
}

/* Wait for the output to drain away, as opposed to flushing
   (discarding) it.  */

static int
hardwire_drain_output (struct serial *scb)
{
#ifdef HAVE_TERMIOS
  return tcdrain (scb->fd);
#endif

#ifdef HAVE_TERMIO
  return ioctl (scb->fd, TCSBRK, 1);
#endif

#ifdef HAVE_SGTTY
  /* Get the current state and then restore it using TIOCSETP,
     which should cause the output to drain and pending input
     to be discarded.  */
  {
    struct hardwire_ttystate state;

    if (get_tty_state (scb, &state))
      {
	return (-1);
      }
    else
      {
	return (ioctl (scb->fd, TIOCSETP, &state.sgttyb));
      }
  }
#endif
}

static int
hardwire_flush_output (struct serial *scb)
{
#ifdef HAVE_TERMIOS
  return tcflush (scb->fd, TCOFLUSH);
#endif

#ifdef HAVE_TERMIO
  return ioctl (scb->fd, TCFLSH, 1);
#endif

#ifdef HAVE_SGTTY
  /* This flushes both input and output, but we can't do better.  */
  return ioctl (scb->fd, TIOCFLUSH, 0);
#endif
}

static int
hardwire_flush_input (struct serial *scb)
{
  ser_base_flush_input (scb);

#ifdef HAVE_TERMIOS
  return tcflush (scb->fd, TCIFLUSH);
#endif

#ifdef HAVE_TERMIO
  return ioctl (scb->fd, TCFLSH, 0);
#endif

#ifdef HAVE_SGTTY
  /* This flushes both input and output, but we can't do better.  */
  return ioctl (scb->fd, TIOCFLUSH, 0);
#endif
}

static int
hardwire_send_break (struct serial *scb)
{
#ifdef HAVE_TERMIOS
  return tcsendbreak (scb->fd, 0);
#endif

#ifdef HAVE_TERMIO
  return ioctl (scb->fd, TCSBRK, 0);
#endif

#ifdef HAVE_SGTTY
  {
    int status;

    status = ioctl (scb->fd, TIOCSBRK, 0);

    /* Can't use usleep; it doesn't exist in BSD 4.2.  */
    /* Note that if this gdb_select() is interrupted by a signal it will not
       wait the full length of time.  I think that is OK.  */
    gdb_usleep (250000);
    status = ioctl (scb->fd, TIOCCBRK, 0);
    return status;
  }
#endif
}

static void
hardwire_raw (struct serial *scb)
{
  struct hardwire_ttystate state;

  if (get_tty_state (scb, &state))
    fprintf_unfiltered (gdb_stderr, "get_tty_state failed: %s\n",
			safe_strerror (errno));

#ifdef HAVE_TERMIOS
  state.termios.c_iflag = 0;
  state.termios.c_oflag = 0;
  state.termios.c_lflag = 0;
  state.termios.c_cflag &= ~CSIZE;
  state.termios.c_cflag |= CLOCAL | CS8;
#ifdef CRTSCTS
  /* h/w flow control.  */
  if (serial_hwflow)
    state.termios.c_cflag |= CRTSCTS;
  else
    state.termios.c_cflag &= ~CRTSCTS;
#ifdef CRTS_IFLOW
  if (serial_hwflow)
    state.termios.c_cflag |= CRTS_IFLOW;
  else
    state.termios.c_cflag &= ~CRTS_IFLOW;
#endif
#endif
  state.termios.c_cc[VMIN] = 0;
  state.termios.c_cc[VTIME] = 0;
#endif

#ifdef HAVE_TERMIO
  state.termio.c_iflag = 0;
  state.termio.c_oflag = 0;
  state.termio.c_lflag = 0;
  state.termio.c_cflag &= ~CSIZE;
  state.termio.c_cflag |= CLOCAL | CS8;
  state.termio.c_cc[VMIN] = 0;
  state.termio.c_cc[VTIME] = 0;
#endif

#ifdef HAVE_SGTTY
  state.sgttyb.sg_flags |= RAW | ANYP;
  state.sgttyb.sg_flags &= ~(CBREAK | ECHO);
#endif

  if (set_tty_state (scb, &state))
    fprintf_unfiltered (gdb_stderr, "set_tty_state failed: %s\n",
			safe_strerror (errno));
}

#ifndef B19200
#define B19200 EXTA
#endif

#ifndef B38400
#define B38400 EXTB
#endif

/* Translate baud rates from integers to damn B_codes.  Unix should
   have outgrown this crap years ago, but even POSIX wouldn't buck it.  */

static struct
{
  int rate;
  int code;
}
baudtab[] =
{
  {
    50, B50
  }
  ,
  {
    75, B75
  }
  ,
  {
    110, B110
  }
  ,
  {
    134, B134
  }
  ,
  {
    150, B150
  }
  ,
  {
    200, B200
  }
  ,
  {
    300, B300
  }
  ,
  {
    600, B600
  }
  ,
  {
    1200, B1200
  }
  ,
  {
    1800, B1800
  }
  ,
  {
    2400, B2400
  }
  ,
  {
    4800, B4800
  }
  ,
  {
    9600, B9600
  }
  ,
  {
    19200, B19200
  }
  ,
  {
    38400, B38400
  }
  ,
#ifdef B57600
  {
    57600, B57600
  }
  ,
#endif
#ifdef B115200
  {
    115200, B115200
  }
  ,
#endif
#ifdef B230400
  {
    230400, B230400
  }
  ,
#endif
#ifdef B460800
  {
    460800, B460800
  }
  ,
#endif
  {
    -1, -1
  }
  ,
};

static int
rate_to_code (int rate)
{
  int i;

  for (i = 0; baudtab[i].rate != -1; i++)
    {
      /* test for perfect macth.  */
      if (rate == baudtab[i].rate)
        return baudtab[i].code;
      else
        {
	  /* check if it is in between valid values.  */
          if (rate < baudtab[i].rate)
	    {
	      if (i)
	        {
	          warning (_("Invalid baud rate %d.  "
			     "Closest values are %d and %d."),
			   rate, baudtab[i - 1].rate, baudtab[i].rate);
		}
	      else
	        {
	          warning (_("Invalid baud rate %d.  Minimum value is %d."),
			   rate, baudtab[0].rate);
		}
	      return -1;
	    }
        }
    }
 
  /* The requested speed was too large.  */
  warning (_("Invalid baud rate %d.  Maximum value is %d."),
            rate, baudtab[i - 1].rate);
  return -1;
}

static int
hardwire_setbaudrate (struct serial *scb, int rate)
{
  struct hardwire_ttystate state;
  int baud_code = rate_to_code (rate);
  
  if (baud_code < 0)
    {
      /* The baud rate was not valid.
         A warning has already been issued.  */
      errno = EINVAL;
      return -1;
    }

  if (get_tty_state (scb, &state))
    return -1;

#ifdef HAVE_TERMIOS
  cfsetospeed (&state.termios, baud_code);
  cfsetispeed (&state.termios, baud_code);
#endif

#ifdef HAVE_TERMIO
#ifndef CIBAUD
#define CIBAUD CBAUD
#endif

  state.termio.c_cflag &= ~(CBAUD | CIBAUD);
  state.termio.c_cflag |= baud_code;
#endif

#ifdef HAVE_SGTTY
  state.sgttyb.sg_ispeed = baud_code;
  state.sgttyb.sg_ospeed = baud_code;
#endif

  return set_tty_state (scb, &state);
}

static int
hardwire_setstopbits (struct serial *scb, int num)
{
  struct hardwire_ttystate state;
  int newbit;

  if (get_tty_state (scb, &state))
    return -1;

  switch (num)
    {
    case SERIAL_1_STOPBITS:
      newbit = 0;
      break;
    case SERIAL_1_AND_A_HALF_STOPBITS:
    case SERIAL_2_STOPBITS:
      newbit = 1;
      break;
    default:
      return 1;
    }

#ifdef HAVE_TERMIOS
  if (!newbit)
    state.termios.c_cflag &= ~CSTOPB;
  else
    state.termios.c_cflag |= CSTOPB;	/* two bits */
#endif

#ifdef HAVE_TERMIO
  if (!newbit)
    state.termio.c_cflag &= ~CSTOPB;
  else
    state.termio.c_cflag |= CSTOPB;	/* two bits */
#endif

#ifdef HAVE_SGTTY
  return 0;			/* sgtty doesn't support this */
#endif

  return set_tty_state (scb, &state);
}

/* Implement the "setparity" serial_ops callback.  */

static int
hardwire_setparity (struct serial *scb, int parity)
{
  struct hardwire_ttystate state;
  int newparity = 0;

  if (get_tty_state (scb, &state))
    return -1;

  switch (parity)
    {
    case GDBPARITY_NONE:
      newparity = 0;
      break;
    case GDBPARITY_ODD:
      newparity = PARENB | PARODD;
      break;
    case GDBPARITY_EVEN:
      newparity = PARENB;
      break;
    default:
      internal_warning (__FILE__, __LINE__,
			"Incorrect parity value: %d", parity);
      return -1;
    }

#ifdef HAVE_TERMIOS
  state.termios.c_cflag &= ~(PARENB | PARODD);
  state.termios.c_cflag |= newparity;
#endif

#ifdef HAVE_TERMIO
  state.termio.c_cflag &= ~(PARENB | PARODD);
  state.termio.c_cflag |= newparity;
#endif

#ifdef HAVE_SGTTY
  return 0;            /* sgtty doesn't support this */
#endif
  return set_tty_state (scb, &state);
}


static void
hardwire_close (struct serial *scb)
{
  if (scb->fd < 0)
    return;

  close (scb->fd);
  scb->fd = -1;
}



/* The hardwire ops.  */

static const struct serial_ops hardwire_ops =
{
  "hardwire",
  hardwire_open,
  hardwire_close,
  NULL,
  ser_base_readchar,
  ser_base_write,
  hardwire_flush_output,
  hardwire_flush_input,
  hardwire_send_break,
  hardwire_raw,
  hardwire_get_tty_state,
  hardwire_copy_tty_state,
  hardwire_set_tty_state,
  hardwire_print_tty_state,
  hardwire_noflush_set_tty_state,
  hardwire_setbaudrate,
  hardwire_setstopbits,
  hardwire_setparity,
  hardwire_drain_output,
  ser_base_async,
  ser_unix_read_prim,
  ser_unix_write_prim
};

void
_initialize_ser_hardwire (void)
{
  serial_add_interface (&hardwire_ops);

#ifdef HAVE_TERMIOS
#ifdef CRTSCTS
  add_setshow_boolean_cmd ("remoteflow", no_class,
			   &serial_hwflow, _("\
Set use of hardware flow control for remote serial I/O."), _("\
Show use of hardware flow control for remote serial I/O."), _("\
Enable or disable hardware flow control (RTS/CTS) on the serial port\n\
when debugging using remote targets."),
			   NULL,
			   show_serial_hwflow,
			   &setlist, &showlist);
#endif
#endif
}

int
ser_unix_read_prim (struct serial *scb, size_t count)
{
  return read (scb->fd, scb->buf, count);
}

int
ser_unix_write_prim (struct serial *scb, const void *buf, size_t len)
{
  return write (scb->fd, buf, len);
}
