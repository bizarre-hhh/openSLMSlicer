#ifndef SLICE_REPORT_EXPORT_H
#define SLICE_REPORT_EXPORT_H

#include "slice_export.h"

#include <cstddef>

// Contact-sheet layout shared by the SVG and PNG reports. Non-empty layers are
// placed row-major into cells; the cell size shrinks automatically so that very
// tall builds still fit into a bounded raster size.
struct SliceReportLayout
{
    int columns = 1;
    int rows = 1;
    int cell_px = 120;
    int width_px = 120;
    int height_px = 120;
};

SliceReportLayout ComputeSliceReportLayout(std::size_t layer_count);

// Vector contact sheet: one SVG file, each non-empty layer is a cell with its
// contours/fills and a "L<grid> z=<mm>" label. Defined in
// slice_report_export.cpp.
bool WriteSvgReport(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result);

// Raster contact sheet with the same layout as the SVG report. The PNG is
// encoded directly (palette + zlib/qCompress) so exporting does not depend on
// the Qt image-format plugins being deployed.
bool WritePngReport(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result);

// Per-layer statistics table for spreadsheets, simulation input and CI diffing.
bool WriteCsvReport(const SliceBuildSnapshot& snapshot,
                    const SliceExportOptions& options,
                    const SliceExportProgress& progress,
                    SliceExportResult& result);

// Read-back helpers used by the post-write validation. Each one validates the
// container structure and returns the counts/lengths stored in the file.
bool SummarizeSvgReport(const QString& path, SliceFileSummary& summary,
                        QString& error);
bool SummarizePngReport(const QString& path, SliceFileSummary& summary,
                        QString& error);
bool SummarizeCsvReport(const QString& path, SliceFileSummary& summary,
                        QString& error);

#endif
