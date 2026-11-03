/*
 * Pinnacle Studio 500-USB open driver
 * Copyright (C) 2026 Jonas Cz.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

package jonascz.marvincapture;

/** The About box: versions, the licence, the bundled libraries and the firmware note. */
final class About {
    private About() {}

    static final String SOURCE_URL = "https://github.com/JonasCz/MarvinCapture";

    static String text() {
        return Core.versionString() + " (API " + Core.apiVersion() + ")\n\n"
                + "Open driver and capture app for the Pinnacle Studio 500-USB family: DV and HDV over "
                + "FireWire, S-Video and composite.\n\n"
                + "Copyright (C) 2026 Jonas Cz. Free software under the GNU Affero General Public License v3.0 "
                + "(AGPL-3.0): you may use, study, share and change it; a changed version you offer to others "
                + "must come with its source. No warranty.\n\n"
                + "Source code: " + SOURCE_URL + "\n\n"
                + "Contains, linked statically:\n"
                + "• FFmpeg 8.1.3 (libavcodec, libavformat, libavfilter, libavutil), LGPL-2.1-or-later, "
                + "https://ffmpeg.org\n"
                + "• libusb 1.0.30, LGPL-2.1-or-later, https://libusb.info\n"
                + "Their unmodified sources and the scripts that build them are named in the repository "
                + "(third_party/README.md); with the app's own source you can rebuild and relink them.\n\n"
                + "Firmware: the capture box's FPGA bitstreams (fpga-ohci.bin, fpga-capture.bin) are "
                + "Pinnacle Systems' copyright, not this project's, and are not covered by its licence. They are "
                + "included only because the hardware does nothing without them and has been discontinued "
                + "since around 2005 (firmware/README.md in the repository).";
    }
}
