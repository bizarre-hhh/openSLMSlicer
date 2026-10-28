#ifndef SLMSLICE_FORMAT_H
#define SLMSLICE_FORMAT_H

#include "slice_export.h"

// Native .slmslice v1 reader/writer. Defined in slmslice_format.cpp.
bool WriteSlmSliceV1(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result);
bool ReadSlmSliceV1(const QString& path, SliceBuildSnapshot& snapshot,
                   SliceFileSummary& summary, QString& error);
bool ReadSlmSliceV1Info(const QString& path, SliceFileInfo& info,
                       QString& error);

#endif
