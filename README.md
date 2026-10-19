# c6g3 Infotainment Services

**c6g3 Infotainment Services** is a lightweight, plug-and-play companion app for [BeamNG.drive](https://www.beamng.com/). It runs quietly in the background on your PC to power next-generation in-cabin screens, audio players, and voice assistants in **c6g3 vehicle mods** (such as the Marcopolo Paradiso G8 1800 DD).

---

## Why is this App Needed for c6g3 Mods?

BeamNG.drive is one of the most advanced vehicle physics simulators in the world. However, the in-game dashboard screens run inside a lightweight embedded web browser (CEF) that has natural limitations:

1. **Play Any Audio Format**  
   The built-in game screen cannot natively play many popular personal media formats. This service bridges that gap, reading your audio files and smoothly feeding them to your dashboard display.

2. **Keep Your Files Where They Are (No Duplicate Storage)**  
   Without this service, you would need to copy gigabytes of music files directly into BeamNG's user folder. With **c6g3 Infotainment Services**, you can simply create a regular Windows folder shortcut (`.lnk`) pointing to your existing music drive or folder, and the vehicle browses it instantly.

3. **Smooth In-Game Performance**  
   Heavy tasks like loading media playlists, background audio decoding, and generating realistic speech happen outside BeamNG's game engine. Your driving physics, steering responsiveness, and frame rates stay fluid with zero micro-stutters.

4. **Next-Gen In-Cabin Voice Assistant**  
   Powers lifelike AI voice synthesis directly in the vehicle cockpit, announcing route stops, navigation prompts, and vehicle diagnostics with expressive, natural human voices.

5. **Fast Satellite Minimap Loading**  
   Automatically optimizes high-resolution satellite map captures into fast, lightweight textures so your GPS map navigation opens instantly without stuttering.

---

## Key Features

* **Personal Audio Player:** Browse and listen to your MP3, FLAC, and WAV music collections with full metadata and album cover art.
* **Windows Shortcut (.lnk) Support:** Drop Windows shortcuts to any folder on any hard drive—the infotainment system resolves them seamlessly.
* **Natural Voice Navigation & Assistant:** Announcements, passenger alerts, and stop notifications delivered through natural voice channels.
* **Zero-Lag Architecture:** Runs as a lightweight background service at below-normal CPU priority, ensuring BeamNG always receives maximum computer power.
* **100% Private & Local:** Runs strictly on your local PC (`127.0.0.1`). Nothing is sent to external servers or the cloud.

---

## How to Use (Quick Start)

Using the service is completely plug-and-play—no complicated installation or configuration required.

### Step 1: Launch the Companion Service
Before or after starting BeamNG.drive, open the `c6g3-infotainment-services` folder and double-click:
```text
c6g3-infotainment-services.exe
```
A console window will appear confirming that the service is running and listening on localhost. You can minimize this window to your taskbar while you drive.

### Step 2: Start BeamNG.drive
Load any map and spawn your favorite c6g3 vehicle (e.g., the **Marcopolo Paradiso G8 1800 DD**).

### Step 3: Enjoy the Dashboard
Turn on the vehicle ignition and press the power button on the infotainment touchscreen. Your music, playlists, voice assistant, and navigation features will automatically connect and work seamlessly.

---

## Adding Your Music

You can access your media in two convenient ways:

### Option A: The Windows Shortcut Way (Recommended)
1. Go to your BeamNG user folder:  
   `%USERPROFILE%\AppData\Local\BeamNG\BeamNG.drive\current\`
2. Open or create the `infotainment/media/` folder.
3. Right-click any folder on your PC with your music (for example, `D:\My Music`), select **Create Shortcut**, and move that shortcut into the `infotainment/media/` folder.
4. Your vehicle screen will automatically detect the shortcut and browse your library directly without taking up extra space on your primary drive.

### Option B: Direct Copy
Simply copy your audio tracks (`.mp3`, `.ogg`, `.flac`, `.wav`) or pictures directly into:  
`%USERPROFILE%\AppData\Local\BeamNG\BeamNG.drive\current\infotainment\media\`

---

## Frequently Asked Questions (FAQ)

#### Do I need to install Python, MSYS2, or third-party runtimes?
**No.** The distributed version is completely self-contained. All necessary media codecs and runtime libraries are packaged directly inside the folder.

#### What happens if I forget to launch the service?
Your vehicle will still drive normally, and standard vehicle gauges (speedometer, fuel, engine temperatures) will still work on the display. However, features requiring external media (music playlists, voice assistant) will display a waiting state until the companion service is started.

#### How do I stop or close the service?
Simply close the `c6g3-infotainment-services.exe` console window or press `Ctrl + C` inside it when you are finished playing.

#### Is my personal data safe?
**Yes.** The service strictly binds to local loopback (`127.0.0.1`) and only communicates directly with your BeamNG game instance. It does not transmit telemetry, personal files, or data to the internet.

---

*Developed for the c6g3 vehicle series for BeamNG.drive.*