{
	"targets": [{
		"target_name": "iohook",
		"win_delay_load_hook": "true",
		"type": "loadable_module",
		"sources": [
			"src/iohook.cc"
		],
		"dependencies": [
			"./uiohook.gyp:uiohook"
		],
		"cflags": [
			"-std=c99"
		],
		"link_settings": {
				"libraries": [
						"-Wl,-rpath,@executable_path/.",
						"-Wl,-rpath,@loader_path/.",
						"-Wl,-rpath,<!(pwd)/build/Release/"
				]
		},
		"include_dirs": [
			"<!@(node -p \"require('node-addon-api').include\")",
			"libuiohook/include"
		],
		"configurations": {
			"Release": {
			}
		}
	}]
}
