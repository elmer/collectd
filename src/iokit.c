/**
 * collectd - src/iokit.c
 * Copyright (C) 2006  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "utils_debug.h"

#define MODULE_NAME "iokit"

#if HAVE_CTYPE_H
#  include <ctype.h>
#endif
#if HAVE_MACH_MACH_TYPES_H
#  include <mach/mach_types.h>
#endif
#if HAVE_MACH_MACH_INIT_H
#  include <mach/mach_init.h>
#endif
#if HAVE_MACH_MACH_ERROR_H
#  include <mach/mach_error.h>
#endif
#if HAVE_COREFOUNDATION_COREFOUNDATION_H
#  include <CoreFoundation/CoreFoundation.h>
#endif
#if HAVE_IOKIT_IOKITLIB_H
#  include <IOKit/IOKitLib.h>
#endif
#if HAVE_IOKIT_IOTYPES_H
#  include <IOKit/IOTypes.h>
#endif

#if HAVE_IOKIT_IOKITLIB_H
# define IOKIT_HAVE_READ 1
#else
# define IOKIT_HAVE_READ 0
#endif

#if IOKIT_HAVE_READ
static mach_port_t io_master_port;
#endif

static char *temperature_file = "temperature-%s.rrd";

static char *ds_def[] =
{
	"DS:value:GAUGE:"COLLECTD_HEARTBEAT":U:U",
	NULL
};
static int ds_num = 1;

static void iokit_init (void)
{
#if IOKIT_HAVE_READ
	kern_return_t status;
	
	/* FIXME: de-allocate port if it's defined */

	status = IOMasterPort (MACH_PORT_NULL, &io_master_port);
	if (status != kIOReturnSuccess)
	{
		syslog (LOG_ERR, "IOMasterPort failed: %s",
				mach_error_string (status));
		io_master_port = MACH_PORT_NULL;
		return;
	}
#endif

	return;
}

static void temperature_write (char *host, char *inst, char *val)
{
	rrd_update_file (host, temperature_file, val, ds_def, ds_num);
}

#if IOKIT_HAVE_READ
static void iokit_submit (char *type, char *inst, double value)
{
	char buf[128];

	if (snprintf (buf, 1024, "%u:%f", (unsigned int) curtime,
				value) >= 128)
		return;

	plugin_submit (type, inst, buf);
}

static void iokit_read (void)
{
	kern_return_t   status;
	io_iterator_t   iterator;
	io_object_t     io_obj;
	CFMutableDictionaryRef prop_dict;
	CFTypeRef       property;

	char   type[128];
	char   inst[128];
	int    value_int;
	double value_double;
	int    i;

	if (!io_master_port || (io_master_port == MACH_PORT_NULL))
		return;

	status = IOServiceGetMatchingServices (io_master_port,
		       	IOServiceNameMatching("IOHWSensor"),
		       	&iterator);
	if (status != kIOReturnSuccess)
       	{
		syslog (LOG_ERR, "IOServiceGetMatchingServices failed: %s",
				mach_error_string (status));
		return;
	}

	while ((io_obj = IOIteratorNext (iterator)))
	{
		prop_dict = NULL;
		status = IORegistryEntryCreateCFProperties (io_obj,
				&prop_dict,
				kCFAllocatorDefault,
				kNilOptions);
		if (status != kIOReturnSuccess)
		{
			DBG ("IORegistryEntryCreateCFProperties failed: %s",
					mach_error_string (status));
			continue;
		}

		/* Copy the sensor type. */
		property = NULL;
		if (!CFDictionaryGetValueIfPresent (prop_dict,
					CFSTR ("type"),
					&property))
			continue;
		if (CFGetTypeID (property) != CFStringGetTypeID ())
			continue;
		if (!CFStringGetCString (property,
					type, 128,
					kCFStringEncodingASCII))
			continue;
		type[127] = '\0';

		/* Copy the sensor location. This will be used as `instance'. */
		property = NULL;
		if (!CFDictionaryGetValueIfPresent (prop_dict,
					CFSTR ("location"),
					&property))
			continue;
		if (CFGetTypeID (property) != CFStringGetTypeID ())
			continue;
		if (!CFStringGetCString (property,
					inst, 128,
					kCFStringEncodingASCII))
			continue;
		inst[127] = '\0';
		for (i = 0; i < 128; i++)
		{
			if (inst[i] == '\0')
				break;
			else if (isalnum (inst[i]))
				inst[i] = (char) tolower (inst[i]);
			else
				inst[i] = '_';
		}

		/* Get the actual value. Some computation, based on the `type'
		 * is neccessary. */
		property = NULL;
		if (!CFDictionaryGetValueIfPresent (prop_dict,
					CFSTR ("current-value"),
					&property))
			continue;
		if (CFGetTypeID (property) != CFNumberGetTypeID ())
			continue;
		if (!CFNumberGetValue (property,
				       	kCFNumberIntType,
				       	&value_int))
			continue;

		if ((strcmp (type, "temperature") == 0)
				|| (strcmp (type, "fanspeed") == 0)
				|| (strcmp (type, "voltage") == 0))
		{
			value_double = ((double) value_int) / 65536.0;
		}
		else
		{
			value_double = (double) value_int;
		}

		/* Do stuff */
		DBG ("type = %s, inst = %s, value_int = %i, value_double = %f",
				type, inst, value_int, value_double);
		iokit_submit (type, inst, value_double);

		CFRelease (prop_dict);
		IOObjectRelease (io_obj);
	} /* while (iterator) */

	IOObjectRelease (iterator);
}
#else
# define iokit_read NULL
#endif /* IOKIT_HAVE_READ */

void module_register (void)
{
	DBG ("IOKIT_HAVE_READ = %i", IOKIT_HAVE_READ);

	plugin_register (MODULE_NAME, iokit_init, iokit_read, NULL);
	plugin_register ("iokit-temperature", NULL, NULL, temperature_write);
}

#undef MODULE_NAME
