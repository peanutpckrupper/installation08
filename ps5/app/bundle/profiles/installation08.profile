; Installation 08: the Installation 08 app's one game (copied to
; /data/installation08/profiles the first time the app starts).
[application]
id = installation08
name = Installation 08
executable = C:\Games\Installation08\installation08.exe
working_directory = C:\Games\Installation08
arguments = --mcc Z:\data\installation08\mcc
dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi,xaudio2_9,ucrtbase=n
prefix = installation08
runtime = wine-wow64
architecture = pe64
graphics = opengl

[display]
desktop = 1920x1080
scaling = fit

[input]
mode = xinput
