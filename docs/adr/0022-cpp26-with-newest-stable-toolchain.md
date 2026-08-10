# 使用 C++26 与最新稳定工具链

Status: accepted

Windows 界面宿主与核心都以 C++26 为目标编译模式，界面使用 C++/WinRT 和 XAML；WinUI 3、Windows App SDK、编译器、SDK 和依赖优先采用实施时最新的稳定版本。新技术必须同时满足 Windows 10 22H2 目标、自包含发行及 x64/ARM64 可靠支持；工具链尚未稳定支持的特性暂缓使用，不能为了新颖性进入持久格式和稳定接口。
