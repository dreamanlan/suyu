// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include <map>
#include <string>

#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckbox>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QTimer>
#include <QImage>
#include "common/common_types.h"

namespace Core {
    class System;
}
namespace Common {
    class ParamPackage;
}
namespace InputCommon {
    class InputSubsystem;
}
class GRenderWindow;

//class BraceApiProvider;
class DataAnalystWidget : public QDockWidget {
    Q_OBJECT

    friend class BraceApiProvider;
public:
    explicit DataAnalystWidget(Core::System& system_, std::shared_ptr<InputCommon::InputSubsystem> input_subsystem_, GRenderWindow* renderWindow_, QWidget* parent = nullptr);
    ~DataAnalystWidget() override;

public slots:
    void OnUpdate();
    void OnRunScript();
    void OnExecCmd();
    void OnEnableStateChanged(int);
    void OnClearAll();
    void OnAddSniffing();
    void OnKeepUnchanged();
    void OnKeepChanged();
    void OnKeepIncreased();
    void OnKeepDecreased();
    void OnRollback();
    void OnUnrollback();
    void OnSaveAbs();
    void OnSaveRel();
    void OnScriptBtn1();
    void OnScriptBtn2();
    void OnScriptBtn3();
    void OnScriptBtn4();
    void OnKeyPress(int modifier, int key);
    void OnKeyRelease(int modifier, int key);
    void OnMousePress(int x, int y, int button);
    void OnMouseRelease(int button);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(int x, int y);
    void OnTouchPress(int x, int y, int id);
    void OnTouchUpdateBegin();
    void OnTouchMove(int x, int y, int id);
    void OnTouchUpdateEnd();
    void OnTouchEnd();
public:
    Core::System& GetSystem()const { return system; };
    void InitCmdDocs();
    void ShowHelp(const std::string& filter)const;
    void AddLog(const std::string& info);
    void Reset();
    void EnableSniffer();
    void DisableSniffer();
    uint32_t GetPixel(int x, int y)const;
    bool GetCursorPos(int& x, int& y)const;
    bool GetScreenSize(int& x, int& y)const;
    std::string ReadButtonParam(int index)const;
    std::string ReadStickParam(int index)const;
    std::string ReadMotionParam(int index)const;
    void ReadParamPackage(const std::string& str)const;
    bool HasParam(const std::string& key)const;
    int GetIntParam(const std::string& key, int def)const;
    float GetFloatParam(const std::string& key, float def)const;
    std::string GetStrParam(const std::string& key, const std::string& def)const;
    void KeyPress(int modifier, int key);
    void KeyRelease(int modifier, int key);
    void MousePress(int x, int y, int button);
    void MouseRelease(int button);
    void MouseMove(int x, int y);
    void MouseWheelChange(int x, int y);
    void TouchPress(int x, int y, int id);
    void TouchUpdateBegin();
    void TouchMove(int x, int y, int id);
    void TouchUpdateEnd();
    void TouchEnd();
    bool GetButtonState(int button_id);
    void SetButtonState(std::size_t player_index, int button_id, bool value);
    void SetStickPosition(std::size_t player_index, int axis_id, float x_value, float y_value);
    void SetMotionState(std::size_t player_index, u64 delta_timestamp, float gyro_x, float gyro_y,
        float gyro_z, float accel_x, float accel_y, float accel_z);
protected:
    virtual void showEvent(QShowEvent* ev) override;
    virtual void hideEvent(QHideEvent* ev) override;
    virtual void closeEvent(QCloseEvent* event) override;
private:
    void ClearResultList();
    void SetSniffingScope(const std::string& sectionId);
    void RefreshMemoryArgs();
    void RefreshResultList(const char* tag) {
        RefreshResultList(tag, false);
    }
    void RefreshResultList(const char* tag, bool full);
    void RemoveExcessResults();
    void CaptureScreen();
    void FocusRenderWindow();
    void ShowButtonParam(int index);
    void ShowStickParam(int index);
    void ShowMotionParam(int index);
    void ShowInputState();
    void SaveResultList(const std::string& file_path) const;
private:
    Core::System& system;
    std::shared_ptr<InputCommon::InputSubsystem> inputSubSystem;
    QTimer updateTimer;
    GRenderWindow* renderWindow;
    QImage screenImage;
    uint64_t lastTime;
    uint64_t screenCaptureInterval;
    bool captureEnabled;
    bool logCaptureTimeConsuming;
    int mouseX;
    int mouseY;
    int maxResultList;
    int maxRecords;
    int maxHistories;
    int maxRollbacks;
    std::shared_ptr<Common::ParamPackage> paramPackage;

    QWidget* dockWidgetContents;
    QCheckBox* enableCheckBox;
    QLineEdit* commandEdit;
    QLineEdit* tagEdit;
    QLabel* scriptInputLabel;
    QLineEdit* scriptInputEdit;
    QPushButton* scriptBtn1;
    QPushButton* scriptBtn2;
    QPushButton* scriptBtn3;
    QPushButton* scriptBtn4;
    QListWidget* listWidget;

    QLineEdit* startAddrEdit;
    QLineEdit* sizeAddrEdit;
    QLineEdit* stepAddrEdit;
    QLineEdit* curValueEdit;
    QLineEdit* pidEdit;

    QVBoxLayout* layout;

    std::map<std::string, std::string> cmdDocs;
};
