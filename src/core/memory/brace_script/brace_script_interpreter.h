#pragma once
#include <string>
#include <variant>
#include <vector>
#include <map>

namespace Core
{
    class System;
}

namespace BraceScriptInterpreter
{
    class IBraceScriptApiProvider
    {
    public:
        virtual ~IBraceScriptApiProvider() {}
    public:
        virtual void LogToView(const std::string& info)const = 0;
        virtual bool ExecCommand(std::string&& cmd, std::string&& arg)const = 0;
        virtual Core::System& GetSystem()const = 0;
        virtual void ShowUI(int ix, int flags)const = 0;
        virtual std::string GetScriptInput()const = 0;
        virtual void SetScriptInputLabel(const std::string& label)const = 0;
        virtual void SetScriptBtnCaption(int index, const std::string& caption)const = 0;
        virtual uint32_t GetPixel(int x, int y)const = 0;
        virtual bool GetCursorPos(int& x, int& y)const = 0;
        virtual bool GetScreenSize(int& x, int& y)const = 0;
        virtual std::string ReadButtonParam(int index)const = 0;
        virtual std::string ReadStickParam(int index)const = 0;
        virtual std::string ReadMotionParam(int index)const = 0;
        virtual void ReadParamPackage(const std::string& str)const = 0;
        virtual bool HasParam(const std::string& key)const = 0;
        virtual int GetIntParam(const std::string& key, int def)const = 0;
        virtual float GetFloatParam(const std::string& key, float def)const = 0;
        virtual std::string GetStrParam(const std::string& key, const std::string& def)const = 0;
        virtual void KeyPress(int modifier, int key)const = 0;
        virtual void KeyRelease(int modifier, int key)const = 0;
        virtual void MousePress(int x, int y, int button)const = 0;
        virtual void MouseRelease(int button)const = 0;
        virtual void MouseMove(int x, int y)const = 0;
        virtual void MouseWheelChange(int x, int y)const = 0;
        virtual void TouchPress(int x, int y, int id)const = 0;
        virtual void TouchUpdateBegin()const = 0;
        virtual void TouchMove(int x, int y, int id)const = 0;
        virtual void TouchUpdateEnd()const = 0;
        virtual void TouchEnd()const = 0;
        virtual bool GetButtonState(int button_id)const = 0;
        virtual void SetButtonState(std::size_t player_index, int button_id, bool value)const = 0;
        virtual void SetStickPosition(std::size_t player_index, int axis_id, float x_value, float y_value)const = 0;
        virtual void SetMotionState(std::size_t player_index, u64 delta_timestamp, float gyro_x, float gyro_y,
            float gyro_z, float accel_x, float accel_y, float accel_z)const = 0;
        virtual void ReplaceSourceShader(uint64_t hash, int stage, std::string&& code)const = 0;
        virtual void ReplaceSpirvShader(uint64_t hash, int stage, std::vector<uint32_t>&& code)const = 0;
    };

    std::string get_exe_path();
    std::string get_absolutely_path(const std::string& path);
    std::string read_file(const std::string& filename);
    bool write_file(const std::string& filename, const std::string& content);
    bool write_file(const std::string& filename, std::string&& content);
    std::vector<std::string> read_file_lines(const std::string& filename);
    bool write_file_lines(const std::string& filename, const std::vector<std::string>& lines);
    std::string trim_string(const std::string& str);
    std::size_t replace_all(std::string& inout, const std::string& what, const std::string& with);
    std::vector<std::string> split_string(const std::string& s, const std::string& delimiters);

    using MessageArg = std::variant<bool, int64_t, uint64_t, double, std::string, std::shared_ptr<void>>;
    using MessageArgs = std::vector<MessageArg>;
    int SplitCmd(const std::string& cmdLine, std::string& first, std::string& second);
    uint64_t GetTimeUs();
    void Init(IBraceScriptApiProvider* pApiProvider);
    const std::map<std::string, std::string>& GetApiDocs();
    bool Send(std::string&& msg);
    bool Send(std::string&& msgId, MessageArgs&& args);
    bool Exec(std::string&& cmdLine);
    bool RunCallback(std::string&& msgId, MessageArgs&& args);
    void Tick();
    void Release();
}