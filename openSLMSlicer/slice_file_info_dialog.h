#ifndef SLICE_FILE_INFO_DIALOG_H
#define SLICE_FILE_INFO_DIALOG_H

#include <QDialog>

#include "slice_export.h"

class QPlainTextEdit;
class QTabWidget;

// Read-only viewer for the .slmslice header/metadata. The dialog keeps a copy of
// the formatted text, not the slice geometry.
class SliceFileInfoDialog : public QDialog
{
public:
    SliceFileInfoDialog(const QString& path, const SliceFileInfo& info,
                        QWidget* parent = nullptr);

private:
    QTabWidget* tabs_ = nullptr;
    QPlainTextEdit* summary_ = nullptr;
    QPlainTextEdit* header_ = nullptr;
    QPlainTextEdit* checksums_ = nullptr;
};

#endif
