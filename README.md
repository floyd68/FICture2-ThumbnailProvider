# FICture2 ThumbnailProvider

Windows Explorer thumbnail handler for DDS texture files.

## Overview

**FICture2 ThumbnailProvider** is a Windows Shell Extension that provides thumbnail previews for DDS (DirectDraw Surface) texture files directly in Windows Explorer. This allows modders and texture artists to quickly browse and identify DDS textures without opening them in a separate application.

## Features

- **Native DDS Support**: Leverages DirectXTex for accurate DDS decoding
- **BCn Format Support**: Correctly handles BC1, BC3, BC4, BC5, BC6H, BC7 compressed textures
- **Stream-Based Loading**: Implements `IInitializeWithStream` for efficient file access
- **Integration with ImageCore**: Uses the same decode pipeline as FICture2 for consistent rendering
- **Windows Explorer Integration**: Thumbnails appear automatically in Explorer views

## Technical Details

### COM Interfaces

- `IThumbnailProvider`: Generates thumbnail bitmaps
- `IInitializeWithStream`: Initializes from file stream data

### Dependencies

- **ImageCore**: Image decoding and format detection library
- **DirectXTex**: Microsoft's texture processing library
- **Windows Imaging Component (WIC)**: For bitmap creation

### Architecture

1. Windows Explorer requests a thumbnail via `IInitializeWithStream::Initialize`
2. File data is read into memory from the provided stream
3. `IThumbnailProvider::GetThumbnail` decodes the image using ImageCore
4. The decoded image is converted to an HBITMAP for Explorer

## Building

### Requirements

- Windows 10/11 SDK
- Visual Studio 2022 with C++ workload
- Platform Toolset v143 or later

### Build Steps

1. Clone the repository:
   ```bash
   git clone https://github.com/floyd68/FICture2-ThumbnailProvider.git
   ```

2. Ensure ImageCore and DirectXTex dependencies are available in the parent FICture2 project

3. Build in Visual Studio:
   - Open `ThumbnailProvider.vcxproj`
   - Select **Release x64** configuration
   - Build the project

4. The DLL will be output to `x64\Release\ThumbnailProvider.dll`

## Installation

### Register the DLL

Run as Administrator:

```cmd
regsvr32 /n /i ThumbnailProvider.dll
```

Or use the exported registration function:

```cmd
regsvr32 ThumbnailProvider.dll
```

### Unregister

```cmd
regsvr32 /u ThumbnailProvider.dll
```

### Registry Configuration

The thumbnail provider is registered for `.dds` files under:

```
HKEY_CURRENT_USER\Software\Classes\.dds\ShellEx\{b824b49d-22ac-4161-ac8a-9916e8fa3f7f}
```

The CLSID `{8b0a3d42-7022-4e35-b45f-7321b3e93c16}` identifies the Ficture2ThumbnailProvider component.

## Troubleshooting

### Thumbnails Not Appearing

1. **Clear the thumbnail cache**:
   ```cmd
   del /f /s /q /a %LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db
   ```

2. **Restart Windows Explorer**:
   - Open Task Manager
   - Find "Windows Explorer"
   - Right-click → Restart

3. **Verify registration**:
   ```cmd
   reg query "HKCU\Software\Classes\.dds\ShellEx\{b824b49d-22ac-4161-ac8a-9916e8fa3f7f}"
   ```

### Debug Build Issues

- Ensure ImageCore.lib and FD2D.lib are built in the same configuration (Debug/Release)
- Check that include paths point to the correct ImageCore headers
- Verify DirectXTex is properly initialized as a submodule

## File Structure

```
ThumbnailProvider/
├── ThumbnailProvider.h         # Provider class declaration
├── ThumbnailProvider.cpp       # Implementation and COM registration
├── ThumbnailProvider.def       # Module definition for exports
├── ThumbnailProvider.vcxproj   # Visual Studio project file
└── README.md                   # This file
```

## Exported Functions

- `DllCanUnloadNow`: Indicates if the DLL can be unloaded
- `DllGetClassObject`: Returns the class factory for the provider
- `DllRegisterServer`: Registers the COM component and file associations
- `DllUnregisterServer`: Unregisters the component

## Integration with FICture2

This thumbnail provider is designed as a companion to [FICture2](https://github.com/floyd68/FICture2), an ultra-fast DDS texture viewer and comparison tool. Both use the same ImageCore decoding pipeline to ensure visual consistency between Explorer thumbnails and the full viewer.

## License

This project is licensed under the MIT License - see the [LICENSE](../LICENSE) file for details.

Copyright (c) 2024 EunSuk, Lee (이은석, floyd)

## Related Projects

- [FICture2](https://github.com/floyd68/FICture2) - Main image viewer application
- [ImageCore](https://github.com/floyd68/ImageCore) - Image decoding library
- [FD2D](https://github.com/floyd68/FD2D) - Direct2D UI framework
- [DirectXTex](https://github.com/microsoft/DirectXTex) - Microsoft's texture library

## Support

For issues or questions, please open an issue on the [FICture2 repository](https://github.com/floyd68/FICture2/issues).
