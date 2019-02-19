/***************************************************************************
 * ROM Properties Page shell extension. (librpbase)                        *
 * RpFile_Kreon.cpp: Standard file object. (Kreon-specific functions)      *
 *                                                                         *
 * Copyright (c) 2016-2019 by David Korth.                                 *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 ***************************************************************************/

#include "RpFile.hpp"
#include "scsi_protocol.h"

#ifdef _WIN32
# include "libwin32common/w32err.h"
# include "win32/RpFile_win32_p.hpp"
// NT DDK SCSI functions.
# if defined(__MINGW32__) && !defined(__MINGW64_VERSION_MAJOR)
// Old MinGW has the NT DDK headers in include/ddk/.
// IOCTL headers conflict with WinDDK.
#  include <ddk/ntddscsi.h>
#  include <ddk/ntddstor.h>
# else
// MinGW-w64 and MSVC has the NT DDK headers in include/.
// IOCTL headers are also required.
#  include <winioctl.h>
#  include <ntddscsi.h>
# endif
#endif

#ifndef _WIN32
# include "RpFile_stdio_p.hpp"
#endif

// SCSI and CD-ROM IOCTLs.
#ifdef __linux__
# include <sys/ioctl.h>
# include <scsi/sg.h>
# include <scsi/scsi.h>
# include <linux/cdrom.h>
#endif /* __linux__ */

// C includes. (C++ namespace)
#include <cassert>

// C++ includes.
#include <vector>
using std::vector;

namespace LibRpBase {

/**
 * Send a SCSI command to the device.
 * @param cdb		[in] SCSI command descriptor block
 * @param cdb_len	[in] Length of cdb
 * @param data		[in/out] Data buffer, or nullptr for SCSI_DIR_NONE operations
 * @param data_len	[in] Length of data
 * @param direction	[in] Data direction
 * @return 0 on success, positive for SCSI sense key, negative for OS error.
 */
int RpFile::scsi_send_cdb(const void *cdb, uint8_t cdb_len,
	void *data, size_t data_len,
	ScsiDirection direction)
{
	int ret = -EIO;

#if defined(_WIN32)
	// SCSI_PASS_THROUGH_DIRECT struct with extra space for sense data.
	struct srb_t {
		SCSI_PASS_THROUGH_DIRECT p;
		struct {
			SCSI_RESP_REQUEST_SENSE s;
			uint8_t b[78];	// Additional sense data. (TODO: Best size?)
		} sense;
	};
	srb_t srb;
	memset(&srb, 0, sizeof(srb));

	// Copy the CDB to the SCSI_PASS_THROUGH structure.
	assert(cdb_len <= sizeof(srb.p.Cdb));
	if (cdb_len > sizeof(srb.p.Cdb)) {
		// CDB is too big.
		return -EINVAL;
	}
	memcpy(srb.p.Cdb, cdb, cdb_len);

	// Data direction and buffer.
	switch (direction) {
		case SCSI_DIR_NONE:
			srb.p.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
			break;
		case SCSI_DIR_IN:
			srb.p.DataIn = SCSI_IOCTL_DATA_IN;
			break;
		case SCSI_DIR_OUT:
			srb.p.DataIn = SCSI_IOCTL_DATA_OUT;
			break;
		default:
			assert(!"Invalid SCSI direction.");
			return -EINVAL;
	}

	// Parameters.
	srb.p.DataBuffer = data;
	srb.p.DataTransferLength = static_cast<ULONG>(data_len);
	srb.p.CdbLength = cdb_len;
	srb.p.Length = sizeof(srb.p);
	srb.p.SenseInfoLength = sizeof(srb.sense);
	srb.p.SenseInfoOffset = offsetof(srb_t, sense.s);
	srb.p.TimeOutValue = 5; // 5-second timeout.

	RP_D(RpFile);
	DWORD dwBytesReturned;
	BOOL bRet = DeviceIoControl(
		d->file, IOCTL_SCSI_PASS_THROUGH_DIRECT,
		(LPVOID)&srb.p, sizeof(srb.p),
		(LPVOID)&srb, sizeof(srb),
		&dwBytesReturned, nullptr);
	if (!bRet) {
		// DeviceIoControl() failed.
		// TODO: Convert Win32 to POSIX?
		return -w32err_to_posix(GetLastError());
	}
	// TODO: Check dwBytesReturned.

	// Check if the command succeeded.
	switch (srb.sense.s.ErrorCode) {
		case SCSI_ERR_REQUEST_SENSE_CURRENT:
		case SCSI_ERR_REQUEST_SENSE_DEFERRED:
			// Error. Return the sense key.
			ret = (srb.sense.s.SenseKey << 16) |
			      (srb.sense.s.AddSenseCode << 8) |
			      (srb.sense.s.AddSenseQual);
			break;

		case SCSI_ERR_REQUEST_SENSE_CURRENT_DESC:
		case SCSI_ERR_REQUEST_SENSE_DEFERRED_DESC:
			// Error, but using descriptor format.
			// Return a generic error.
			ret = -EIO;
			break;

		default:
			// No error.
			ret = 0;
			break;
	}
#elif defined(__linux__)
	// SCSI command buffers.
	struct sg_io_hdr sg_io;
	union {
		struct request_sense s;
		uint8_t u[18];
	} _sense;

	// TODO: Consolidate this.
	memset(&sg_io, 0, sizeof(sg_io));
	sg_io.interface_id = 'S';
	sg_io.mx_sb_len = sizeof(_sense);
	sg_io.sbp = _sense.u;
	sg_io.flags = SG_FLAG_LUN_INHIBIT | SG_FLAG_DIRECT_IO;

	sg_io.cmdp = (unsigned char*)cdb;
	sg_io.cmd_len = cdb_len;

	switch (direction) {
		case SCSI_DIR_NONE:
			sg_io.dxfer_direction = SG_DXFER_NONE;
			break;
		case SCSI_DIR_IN:
			sg_io.dxfer_direction = SG_DXFER_FROM_DEV;
			break;
		case SCSI_DIR_OUT:
			sg_io.dxfer_direction = SG_DXFER_TO_DEV;
			break;
		default:
			assert(!"Invalid SCSI direction.");
			return -EINVAL;
	}
	sg_io.dxferp = data;
	sg_io.dxfer_len = data_len;

	RP_D(RpFile);
	if (ioctl(fileno(d->file), SG_IO, &sg_io) != 0) {
		// ioctl failed.
		return -errno;
	}

	// Check if the command succeeded.
	if ((sg_io.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
		// Command failed.
		ret = -EIO;
		if (sg_io.masked_status & CHECK_CONDITION) {
			ret = ERRCODE(_sense.u);
			if (ret == 0) {
				ret = -EIO;
			}
		}
	}
#else
# error No SCSI implementation for this OS.
#endif
	return ret;
}

/**
 * Is this a supported Kreon drive?
 *
 * NOTE: This only checks the drive vendor and model.
 * Check the feature list to determine if it's actually
 * using Kreon firmware.
 *
 * @return True if the drive supports Kreon firmware; false if not.
 */
bool RpFile::isKreonDriveModel(void)
{
	RP_D(RpFile);
	if (!d->isDevice) {
		// Not a device.
		return false;
	}

	// SCSI INQUIRY command.
	uint8_t cdb[6] = {0x12, 0x00, 0x00,
		sizeof(SCSI_RESP_INQUIRY_STD) >> 8,
		sizeof(SCSI_RESP_INQUIRY_STD) & 0xFF,
		0x00};

	SCSI_RESP_INQUIRY_STD resp;
	int ret = scsi_send_cdb(cdb, sizeof(cdb), &resp, sizeof(resp), SCSI_DIR_IN);
	if (ret != 0) {
		// SCSI command failed.
		return false;
	}

	// Check the drive vendor and product ID.
	if (!memcmp(resp.vendor_id, "TSSTcorp", 8)) {
		// Correct vendor ID.
		// Check for supported product IDs.
		// NOTE: More drive models are supported, but the
		// Kreon firmware only uses these product IDs.
		static const char *const product_id_tbl[] = {
			"DVD-ROM SH-D162C",
			"DVD-ROM TS-H353A",
			"DVD-ROM SH-D163B",
		};
		for (int i = 0; i < ARRAY_SIZE(product_id_tbl); i++) {
			if (!memcmp(resp.product_id, product_id_tbl[i], sizeof(resp.product_id))) {
				// Found a match.
				return true;
			}
		}
	}

	// Drive model is not supported.
	return false;
}

/**
 * Get a list of supported Kreon features.
 * @return List of Kreon feature IDs, or empty vector if not supported.
 */
vector<uint16_t> RpFile::getKreonFeatureList(void)
{
	// NOTE: On Linux, this ioctl will fail if not running as root.
	RP_D(RpFile);
	vector<uint16_t> vec;
	if (!d->isDevice) {
		// Not a device.
		return vec;
	}

	// Kreon "Get Feature List" command
	// Reference: https://github.com/saramibreak/DiscImageCreator/blob/cb9267da4877d32ab68263c25187cbaab3435ad5/DiscImageCreator/execScsiCmdforDVD.cpp#L1223
	uint8_t cdb[6] = {0xFF, 0x08, 0x01, 0x10, 0x00, 0x00};
	uint8_t feature_buf[26];
	int ret = scsi_send_cdb(cdb, sizeof(cdb), feature_buf, sizeof(feature_buf), SCSI_DIR_IN);
	if (ret != 0) {
		// SCSI command failed.
		return vec;
	}

	vec.reserve(sizeof(feature_buf)/sizeof(uint16_t));
	for (size_t i = 0; i < sizeof(feature_buf); i += 2) {
		const uint16_t feature = (feature_buf[i] << 8) | feature_buf[i+1];
		if (feature == 0)
			break;
		vec.push_back(feature);
	}

	if (vec.size() < 2 || vec[0] != KREON_FEATURE_HEADER_0 ||
	    vec[1] != KREON_FEATURE_HEADER_1)
	{
		// Kreon feature list is invalid.
		vec.clear();
		vec.shrink_to_fit();
	}

	return vec;
}

/**
 * Set Kreon error skip state.
 * @param skip True to skip; false for normal operation.
 * @return 0 on success; non-zero on error.
 */
int RpFile::setKreonErrorSkipState(bool skip)
{
	// NOTE: On Linux, this ioctl will fail if not running as root.
	RP_D(RpFile);
	if (!d->isDevice) {
		// Not a device.
		return -ENODEV;
	}

	// Kreon "Set Error Skip State" command
	// Reference: https://github.com/saramibreak/DiscImageCreator/blob/cb9267da4877d32ab68263c25187cbaab3435ad5/DiscImageCreator/execScsiCmdforDVD.cpp#L1341
	uint8_t cdb[6] = {0xFF, 0x08, 0x01, 0x15, (uint8_t)skip, 0x00};
	return scsi_send_cdb(cdb, sizeof(cdb), nullptr, 0, SCSI_DIR_IN);
}

/**
 * Set Kreon lock state
 * @param lockState 0 == locked; 1 == Unlock State 1 (xtreme); 2 == Unlock State 2 (wxripper)
 * @return 0 on success; non-zero on error.
 */
int RpFile::setKreonLockState(uint8_t lockState)
{
	// NOTE: On Linux, this ioctl will fail if not running as root.
	RP_D(RpFile);
	if (!d->isDevice) {
		// Not a device.
		return -ENODEV;
	}

	// Kreon "Set Lock State" command
	// Reference: https://github.com/saramibreak/DiscImageCreator/blob/cb9267da4877d32ab68263c25187cbaab3435ad5/DiscImageCreator/execScsiCmdforDVD.cpp#L1309
	uint8_t cdb[6] = {0xFF, 0x08, 0x01, 0x11, (uint8_t)lockState, 0x00};
	return scsi_send_cdb(cdb, sizeof(cdb), nullptr, 0, SCSI_DIR_IN);
}

}