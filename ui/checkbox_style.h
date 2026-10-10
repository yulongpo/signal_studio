#pragma once

#include <QString>

namespace signalstudio {

// Keep indicator contrast independent of the native Windows style and local
// panel styles, including dialogs and disabled analysis parameter groups.
inline QString checkboxStyleSheet() {
    return QStringLiteral(R"(
        QCheckBox { spacing:7px; }
        QCheckBox::indicator {
            width:14px; height:14px; border:1px solid #7896b2;
            border-radius:3px; background:#213950;
        }
        QCheckBox::indicator:unchecked:hover { border-color:#a7dcf4; background:#2c4d68; }
        QCheckBox::indicator:checked,QCheckBox::indicator:indeterminate {
            border-color:#98dcf6; background:#359ac5;
        }
        QCheckBox::indicator:checked { image:url(:/signalstudio/ui/check.svg); }
        QCheckBox::indicator:indeterminate { image:url(:/signalstudio/ui/check-partial.svg); }
        QCheckBox::indicator:checked:hover,QCheckBox::indicator:indeterminate:hover { background:#48b2df; border-color:#dbf5ff; }
        QCheckBox::indicator:focus { border:2px solid #e4f6ff; }
        QCheckBox::indicator:pressed { background:#237598; }
        QCheckBox:disabled { color:#8299af; }
        QCheckBox::indicator:disabled { border-color:#586f84; background:#243444; }
        QCheckBox::indicator:checked:disabled,QCheckBox::indicator:indeterminate:disabled { background:#426079; }
    )");
}

} // namespace signalstudio
