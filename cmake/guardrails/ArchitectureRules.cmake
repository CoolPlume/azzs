include_guard(GLOBAL)

# This file is the single source of truth for project dependency roles and
# allowed edges. Fixtures describe graphs only; they never repeat this matrix.
set(AZZS_ARCHITECTURE_ROLES
  domain
  application
  platform_adapter
  infrastructure_adapter
  ui
  entrypoint
  composition
  test_support
  test
  runtime
  platform_runtime
)

set(AZZS_ARCHITECTURE_PRODUCTION_ROLES
  domain
  application
  platform_adapter
  infrastructure_adapter
  ui
  entrypoint
  composition
)

# role | repository-relative directory. Project targets may only claim a role
# from its owned directory. More specific entries are checked by exact role.
set(AZZS_ARCHITECTURE_ROLE_DIRECTORIES
  "domain|src/domain"
  "application|src/application"
  "platform_adapter|src/adapters/windows"
  "infrastructure_adapter|src/adapters/infrastructure"
  "ui|src/adapters/ui"
  "entrypoint|src/adapters/ui"
  "composition|src/composition"
  "test_support|tests/support"
  "test|tests"
)

set(AZZS_ARCHITECTURE_COMPOSITION_NODES
  Azzs.WinUI
  src/composition/windows/composition_root.cpp
  src/composition/windows/composition_root.hpp
)

# Exact exceptions are declared here before directory ownership is applied.
set(AZZS_ARCHITECTURE_SOURCE_ROLE_OVERRIDES
  "entrypoint|src/adapters/ui/winui/App.xaml.cpp"
)

set(AZZS_ARCHITECTURE_SCAN_EXCLUDE_PATTERNS
  "^tests/architecture/fixtures/"
)

set(AZZS_ARCHITECTURE_ALLOWED_domain
  domain
  runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_application
  application
  domain
  runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_platform_adapter
  platform_adapter
  application
  domain
  runtime
  platform_runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_infrastructure_adapter
  infrastructure_adapter
  application
  domain
  runtime
  platform_runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_ui
  ui
  application
  runtime
  platform_runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_entrypoint
  entrypoint
  ui
  composition
  runtime
  platform_runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_composition
  composition
  entrypoint
  ui
  application
  domain
  platform_adapter
  infrastructure_adapter
  runtime
  platform_runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_test_support
  test_support
  application
  domain
  runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_test
  test
  test_support
  application
  domain
  platform_adapter
  infrastructure_adapter
  ui
  runtime
  platform_runtime
)
set(AZZS_ARCHITECTURE_ALLOWED_runtime runtime)
set(AZZS_ARCHITECTURE_ALLOWED_platform_runtime platform_runtime)

# target-or-library | graph-node | role
set(AZZS_ARCHITECTURE_EXTERNAL_CMAKE_TARGETS
  "Threads::Threads|external:threads|runtime"
  "advapi32|external:windows-platform|platform_runtime"
  "ole32|external:windows-platform|platform_runtime"
  "oleaut32|external:windows-platform|platform_runtime"
  "rpcrt4|external:windows-platform|platform_runtime"
  "shell32|external:windows-platform|platform_runtime"
  "wbemuuid|external:windows-platform|platform_runtime"
  "winhttp|external:windows-platform|platform_runtime"
  "wtsapi32|external:windows-platform|platform_runtime"
)
set(AZZS_ARCHITECTURE_EXTERNAL_LINK_ITEM_PATTERNS
  "^-pthread$"
  "^pthread$"
)

set(AZZS_ARCHITECTURE_CMAKE_DEPENDENCY_OPTION_PATTERNS
  "(^|[ ;])(-include|-imacros|-I|-isystem)(=|[ ;]|[^ ]+)"
  "(^|[ ;])(/FI|/I|/external:I)"
  "(^|[ ;])(-l[^ ]+|/DEFAULTLIB:|/WHOLEARCHIVE:)"
)
set(AZZS_ARCHITECTURE_EXTERNAL_MSBUILD_LIBRARIES
  "advapi32.lib|external:windows-platform|platform_runtime"
  "ole32.lib|external:windows-platform|platform_runtime"
  "oleaut32.lib|external:windows-platform|platform_runtime"
  "rpcrt4.lib|external:windows-platform|platform_runtime"
  "shell32.lib|external:windows-platform|platform_runtime"
  "wbemuuid.lib|external:windows-platform|platform_runtime"
  "winhttp.lib|external:windows-platform|platform_runtime"
  "wtsapi32.lib|external:windows-platform|platform_runtime"
  "windowsapp.lib|external:windows-platform|platform_runtime"
)

set(AZZS_ARCHITECTURE_EXPECTED_MSBUILD_EDGES
  "Azzs.WinUI|azzs_windows_adapter"
  "Azzs.WinUI|azzs_infrastructure_adapter"
  "Azzs.WinUI|azzs_application"
  "Azzs.WinUI|azzs_domain"
  "Azzs.WinUI|azzs_architecture_selection_application"
  "Azzs.WinUI|azzs_architecture_selection_domain"
  "Azzs.WinUI|external:windows-platform"
)

set(AZZS_ARCHITECTURE_ALLOWED_MSBUILD_IMPORTS
  [==[$(VCTargetsPath)\Microsoft.Cpp.Default.props]==]
  [==[$(VCTargetsPath)\Microsoft.Cpp.props]==]
  [==[$(VCTargetsPath)\Microsoft.Cpp.targets]==]
)

set(AZZS_ARCHITECTURE_ALLOWED_MSBUILD_PACKAGES
  "Microsoft.WindowsAppSDK|2.3.1"
  "Microsoft.Windows.CppWinRT|3.0.260715.1"
)

set(AZZS_ARCHITECTURE_ALLOWED_MSBUILD_COMPILE_DEPENDENCY_ELEMENTS
  AdditionalIncludeDirectories
  PrecompiledHeaderFile
  AdditionalOptions
)
set(AZZS_ARCHITECTURE_REJECTED_MSBUILD_COMPILE_DEPENDENCY_ELEMENTS
  AdditionalUsingDirectories
  ForcedIncludeFiles
  ForcedUsingFiles
)
set(AZZS_ARCHITECTURE_ALLOWED_MSBUILD_ADDITIONAL_OPTIONS
  /Zc:__cplusplus
  /utf-8
)

set(AZZS_ARCHITECTURE_STANDARD_HEADER_PATTERNS
  "^(algorithm|any|array|atomic|barrier|bit|bitset|cassert|cctype|cerrno|cfenv|cfloat|charconv|chrono|cinttypes|climits|clocale|cmath|codecvt|compare|complex|concepts|condition_variable|coroutine|csetjmp|csignal|cstdarg|cstddef|cstdint|cstdio|cstdlib|cstring|ctime|cuchar|cwchar|cwctype|deque|exception|execution|expected|filesystem|format|forward_list|fstream|functional|future|initializer_list|iomanip|ios|iosfwd|iostream|istream|iterator|latch|limits|list|locale|map|memory|memory_resource|mutex|new|numbers|numeric|optional|ostream|print|queue|random|ranges|ratio|regex|scoped_allocator|semaphore|set|shared_mutex|source_location|span|spanstream|sstream|stack|stacktrace|stdexcept|stdfloat|stop_token|streambuf|string|string_view|strstream|syncstream|system_error|thread|tuple|type_traits|typeindex|typeinfo|unordered_map|unordered_set|utility|valarray|variant|vector|version)$"
  "^(assert|ctype|errno|fenv|float|inttypes|limits|locale|math|setjmp|signal|stdarg|stddef|stdint|stdio|stdlib|string|time|uchar|wchar|wctype)\\.h$"
)

set(AZZS_ARCHITECTURE_WINUI_GENERATED_HEADER_PATTERNS
  "(^|/)[A-Za-z0-9_.-]+\\.g\\.(h|cpp)$"
)

set(AZZS_ARCHITECTURE_PLATFORM_HEADER_PATTERNS
  "^win[^/]*\\.h$"
  "^aclapi\\.h$"
  "^knownfolders\\.h$"
  "^rpc\\.h$"
  "^sddl\\.h$"
  "^wtsapi32\\.h$"
  "^oleauto\\.h$"
  "^wbemidl\\.h$"
  "^unknwn\\.h$"
  "^combaseapi\\.h$"
  "^objbase\\.h$"
  "^shellapi\\.h$"
  "^shlobj[^/]*\\.h$"
  "^processthreadsapi\\.h$"
  "^tlhelp32\\.h$"
  "^setupapi\\.h$"
  "^cfgmgr32\\.h$"
  "^dwmapi\\.h$"
  "^uxtheme\\.h$"
  "^fcntl\\.h$"
  "^sys/file\\.h$"
  "^unistd\\.h$"
  "^wrl/"
  "^winrt/"
)

set(AZZS_ARCHITECTURE_NO_DIRECT_SYSTEM_EFFECT_ROLES
  ui
  entrypoint
  composition
)

# These high-confidence patterns prevent UI and composition sources from
# bypassing application interfaces for direct system mutation. Semantic review
# remains required for wrappers and indirect calls.
set(AZZS_ARCHITECTURE_SYSTEM_EFFECT_PATTERNS
  [==[(^|[^A-Za-z0-9_])(CreateFile|WriteFile|DeleteFile|MoveFile|CopyFile|ReplaceFile|CreateDirectory|RemoveDirectory|SetFileAttributes)(A|W)?([^A-Za-z0-9_]|$)]==]
  [==[(^|[^A-Za-z0-9_])(RegCreateKey|RegOpenKey|RegSetValue|RegDeleteKey|RegDeleteTree|RegDeleteValue)(Ex)?(A|W)?([^A-Za-z0-9_]|$)]==]
  [==[(^|[^A-Za-z0-9_])(CreateProcess|ShellExecute|ShellExecuteEx|WinExec)(A|W)?([^A-Za-z0-9_]|$)]==]
  [==[(^|[^A-Za-z0-9_])(system|_wsystem)[ \t]*\(]==]
  [==[&[ \t]*(system|_wsystem)([^A-Za-z0-9_]|$)]==]
  [==[(^|[^A-Za-z0-9_])(OpenSCManager|CreateService|StartService|ControlService|DeleteService)(A|W)?([^A-Za-z0-9_]|$)]==]
  [==[(^|[^A-Za-z0-9_])(WinHttp|InternetOpen|InternetConnect|HttpOpenRequest|URLDownloadToFile)[A-Za-z0-9_]*([^A-Za-z0-9_]|$)]==]
  [==[(^|[^A-Za-z0-9_])(LoadLibrary|GetProcAddress)(Ex)?(A|W)?([^A-Za-z0-9_]|$)]==]
  [==[(^|[^A-Za-z0-9_])(fopen|_wfopen)([^A-Za-z0-9_]|$)]==]
  [==[std::(o?fstream|filesystem::(copy|copy_file|create_directory|create_directories|remove|remove_all|rename|resize_file))([^A-Za-z0-9_]|$)]==]
  [==[(Windows::(Storage|Networking|Management::Deployment)|StorageFile|StorageFolder|PackageManager|HttpClient|BackgroundDownloader)([^A-Za-z0-9_]|$)]==]
)
