#ifndef CLI_FORMAT_H
#define CLI_FORMAT_H

#include "slice_export.h"

// CLI 2.0 ASCII writer and the small independent parser used for round-trip
// validation. Defined in cli_format.cpp.
bool WriteCliAscii(const SliceBuildSnapshot& snapshot,
                   const SliceExportOptions& options,
                   const SliceExportProgress& progress,
                   SliceExportResult& result);
bool ParseCliAscii(const QString& path, SliceFileSummary& summary,
                   QString& error);

#endif
