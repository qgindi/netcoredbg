LA uses the Samsung/netcoredbg debugger: https://github.com/Samsung/netcoredbg
But need many modifications. Below are steps to install netcoredbg sources, modify projects and build.
After completing these steps, the output files already work and can be used, but not everything works well because the source code still not modified.
Most source code modifications are not included here. They are in the private github repo qgindi/netcoredbg.

Ensure you have the prerequisities listed in https://github.com/Samsung/netcoredbg: cmake, git, .NET SDK.

Git clone https://github.com/Samsung/netcoredbg.git to C:\code-lib\netcoredbg. Full, not shallow.

Create and open folder C:\code-lib\netcoredbg\build

Right-click in the folder, open terminal and run:
`cmake .. -G "Visual Studio 17 2022" -DDOTNET_DIR="c:\Program Files\dotnet"`

Edit .gitignore. See below (near the end).

Git commit (rclick > TortoiseGit > Commit). Message: "executed cmake; edited .gitignore". Check all files.

Open C:\code-lib\netcoredbg\build\netcoredbg.sln

Unload projects: ALL_BUILD, buildinfo, example, INSTALL, managedpart_dll, PACKAGE. Make netcoredbg "startup project".

In Configuration Manager:
- Delete solution configurations other than Debug and Release. Note: check "Remove from projects".
- Add solution platform ARM64. Note: don't create project platforms.
- Add platform ARM64 to projects netcoredbg, linenoise, corguids. Set it to build in all config.

Change properties of projects netcoredbg, linenoise and corguids:
- General > Output directory: $(Platform)\$(Configuration)\
- General > Intermediate directory: $(ProjectName).dir\$(Platform)\$(Configuration)\
- In preprocessor of ARM64 platform replace all substrings "AMD" with "ARM".
- Linker > Input > Additional dependencies: for corguids.lib and linenoise.lib use $(OutDir).
- netcoredbg only: Linker > Debugging > Database file: use $(OutDir).
- Linker > All options > Additional options: clear.
- If not lazy, everywhere use variables instead of raw paths.
- netcoredbg Post-Build event for x64 and ARM64:
 
```
C:\code\au\_\Debugger\x64\ /Y
xcopy $(TargetPath) C:\code\au.editor\Debugger\x64\ /Y
```
```
xcopy $(TargetPath) C:\code\au\_\Debugger\arm64\ /Y
xcopy $(TargetPath) C:\code\au.editor\Debugger\arm64\ /Y
```

Create folders:
- C:\code\au\_\Debugger\x64
- C:\code\au\_\Debugger\arm64

Modify "interop.cpp":
below `std::string exeDir = exe.substr(0, dirSepIndex);` insert: `std::string appPaths = exeDir + "\\..;" + exeDir + "\\..\\..\\Roslyn"; //Au: let .NET find ..\ManagedPart.dll and ..\..\Roslyn\dlls` and use appPaths below for APP_PATHS and APP_NI_PATHS. Other mods can be done later; the debugger works without them.

Build netcoredbg.

Add this project to the solution: \src\managed\ManagedPart.csproj. In ManagedPart:
- Remove Debug config.
- Edit csproj: set target platform net9.0; remove all NuGet. Close.
- Add refs to our dlls in _\Roslyn: CodeAnalysis.dll and CodeAnalysis.CSharp.dll.
- Set postbuild: `xcopy $(TargetPath) C:\code\au\_\Debugger\ /Y`
- Build.

Add files from NuGet https://www.nuget.org/packages/Microsoft.Diagnostics.DbgShim.win-x64/arm64:
- C:\code\au\_\Debugger\x64\dbgshim.dll
- C:\code\au\_\Debugger\arm64\dbgshim.dll



---------------

In .gitignore replace /build/ with:
```
# Au
/.*/
/docs/
/packaging/
/test*/
/build/*
!*.vcxproj
!*.sln
!/build/src/
!/build/third_party/
/build/src/*
!*.vcxproj
/build/third_party/*
!/build/third_party/linenoise-ng/
/build/third_party/linenoise-ng/*
!*.vcxproj
*.bak
```

-----------------

This can be used to see what files are ignored:
`git ls-files -io --directory --exclude-standard`

The above does not show already tracked files. This can be used to untrack all:
`git rm --cached -r .`
(including the dot at the end)
