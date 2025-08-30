compile with vc143
1.Change "add_custom_target(clang-format" in externals\cubeb\CMakeLists.txt to "add_custom_target(clang-format-cubed"
2.Modify the opRO_MR function in xbyak.h and add the type T_ALLOW_DIFF_SIZE

delete old boost in vcpgk
1.Delete boost files in externals\vcpgk\downloads
2.Delete boost files in externals\vcpgk\packages
3.Delete boost files in externals\vcpgk\ports
4.Delete boost files in externals\vcpgk\scripts
5.Delete boost files in externals\vcpgk\versions\b-
6.In the root directory of yuzu, execute the command "git submodule update --remote --recursive -f externals/vcpkg"
7.Modify vcpkg.json in the yuzu root directory and add boost lib and version in the dependencies array:

{ "name": "boost", "version>=": "1.88.0" },

or 

{ "name": "boost-algorithm", "version>=": "1.88.0" },
{ "name": "boost-asio", "version>=": "1.88.0" },
{ "name": "boost-bind", "version>=": "1.88.0" },
{ "name": "boost-config", "version>=": "1.88.0" },
{ "name": "boost-container", "version>=": "1.88.0" },
{ "name": "boost-context", "version>=": "1.88.0" },
{ "name": "boost-crc", "version>=": "1.88.0" },
{ "name": "boost-dll", "version>=": "1.88.0" },
{ "name": "boost-filesystem", "version>=": "1.88.0" },
{ "name": "boost-system", "version>=": "1.88.0" },
{ "name": "boost-functional", "version>=": "1.88.0" },
{ "name": "boost-heap", "version>=": "1.88.0" },
{ "name": "boost-icl", "version>=": "1.88.0" },
{ "name": "boost-intrusive", "version>=": "1.88.0" },
{ "name": "boost-mpl", "version>=": "1.88.0" },
{ "name": "boost-process", "version>=": "1.88.0" },
{ "name": "boost-range", "version>=": "1.88.0" },
{ "name": "boost-spirit", "version>=": "1.88.0" },
{ "name": "boost-test", "version>=": "1.88.0" },
{ "name": "boost-timer", "version>=": "1.88.0" },
{ "name": "boost-variant", "version>=": "1.88.0" },

etc.
8.Modify the vcpkg.json in the yuzu root directory and change builtin-baseline to the latest git version of vcpkg
