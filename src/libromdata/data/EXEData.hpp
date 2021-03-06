/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * EXEData.hpp: DOS/Windows executable data.                               *
 *                                                                         *
 * Copyright (c) 2016-2020 by David Korth.                                 *
 * SPDX-License-Identifier: GPL-2.0-or-later                               *
 ***************************************************************************/

#ifndef __ROMPROPERTIES_LIBROMDATA_DATA_EXEDATA_HPP__
#define __ROMPROPERTIES_LIBROMDATA_DATA_EXEDATA_HPP__

#include "common.h"

// C includes.
#include <stdint.h>

namespace LibRomData {

class EXEData
{
	private:
		// Static class.
		EXEData();
		~EXEData();
		RP_DISABLE_COPY(EXEData)

	public:
		/**
		 * Look up a PE machine type. (CPU)
		 * @param cpu PE machine type.
		 * @return Machine type name, or nullptr if not found.
		 */
		static const char *lookup_pe_cpu(uint16_t cpu);

		/**
		 * Look up an LE machine type. (CPU)
		 * @param cpu LE machine type.
		 * @return Machine type name, or nullptr if not found.
		 */
		static const char *lookup_le_cpu(uint16_t cpu);
};

}

#endif /* __ROMPROPERTIES_LIBROMDATA_EXEDATA_HPP__ */
