/*
 * SPDX-FileCopyrightText: Copyright 2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* ----------------------------------------------------------------------
 * Project:      Fast Models Portfolio - MTI Plugin
 * Title:        JsonWriter.h
 * Description:  Streaming Chrome Tracing JSON writer (complete events, signal-safe close)
 *
 * $Date:        22 April 2026
 * $Revision:    V.1.0.0
 *
 * Target:       Arm(R) Fast Models Portfolio (FVP)
 *
 * -------------------------------------------------------------------- */

/* Chrome Tracing format reference:
 *   https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU
 *
 * Events are written to a file one-by-one (streaming), so the plugin never
 * holds the whole trace in memory -- important for long simulation runs.
 *
 * The output file is a valid JSON array even when Finalize() is called
 * early (e.g. after Ctrl-C):
 *   [
 *     {"ph":"X", ...},
 *     {"ph":"X", ...}
 *   ]
 *
 * Thread-safety: NOT thread-safe.  The FVP simulation thread calls TracePC
 * sequentially, so no locking is needed.
 *
 * Signal-safety: EmergencyClose() uses only write(2) and is safe to call
 * from a UNIX signal handler.
 */

#pragma once

#include <cstdint>
#include <cstdio> // FILE*
#include <string>
#include <unistd.h> // write(), fsync()

class JsonWriter {
  public:
    JsonWriter() = default;
    ~JsonWriter();

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /// Open @p path for writing and begin the JSON array.
    /// Returns true on success, false if the file cannot be created.
    bool Open(const std::string &path);

    /// Flush pending I/O, write the closing bracket, and close the file.
    /// Idempotent: safe to call multiple times.
    void Finalize();

    /// Signal-handler-safe version: uses only write(2).
    /// Writes "\n]\n" and calls fsync().  Call this from SIGINT/SIGTERM
    /// handlers instead of Finalize().
    void EmergencyClose();

    bool IsOpen() const { return file_ != nullptr; }

    // ------------------------------------------------------------------
    // Event writers
    // ------------------------------------------------------------------

    /// Write a "Complete" event (duration slice, ph="X").
    ///
    /// \param name         Display name shown in the profiler UI.
    /// \param category     Category string (e.g. "arm", "svc").
    /// \param timestamp_us Start timestamp in microseconds.
    /// \param duration_us  Duration in microseconds.
    /// \param pid          Process ID (groups tracks in the UI).
    /// \param tid          Thread ID (lane within a process group).
    void WriteCompleteEvent(const std::string &name,
                            const std::string &category,
                            double timestamp_us,
                            double duration_us,
                            int pid,
                            int tid);

    /// Write an "Instant" event (ph="I", scope="p" = process level).
    ///
    /// \param name         Display name.
    /// \param category     Category string.
    /// \param timestamp_us Timestamp in microseconds.
    /// \param pid          Process ID.
    void WriteInstantEvent(const std::string &name, const std::string &category, double timestamp_us, int pid);

    // ------------------------------------------------------------------
    // Static helpers
    // ------------------------------------------------------------------

    /// Escape @p s so it is safe to embed inside a JSON string literal.
    /// Handles backslash, double-quote, and ASCII control characters.
    static std::string EscapeJson(const std::string &s);

  private:
    FILE *file_ = nullptr;
    int fd_ = -1;             ///< Kept for EmergencyClose()
    bool first_event_ = true; ///< Controls comma insertion
    bool finalized_ = false;  ///< Guards against double-close

    /// Write the inter-event comma (skipped before the very first event).
    void WriteSeparator();
};
