/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

import Cocoa
import IOKit.pwr_mgt

class CocoaCB: NSObject {
    // Note: MPVHelper is more like VOHelper
    var mpv: MPVHelper?
    var libmpv: LibmpvHelper
    var window: Window?
    var titleBar: TitleBar?
    var view: EventsView?
    var layer: VideoLayer?
    var link: CVDisplayLink?
    var timer: PreciseTimer?

    var cursorHidden: Bool = false
    var cursorVisibilityWanted: Bool = true
    @objc var isShuttingDown: Bool = false

    var title: String = "mpv" {
        didSet { if let window = window { window.title = title } }
    }

    enum State {
        case uninitialized
        case needsInit
        case initialized
    }
    var backendState: State = .uninitialized

    let eventsLock = NSLock()
    var events: Int = 0

    var lightSensor: io_connect_t = 0
    var lastLmu: UInt64 = 0
    var lightSensorIOPort: IONotificationPortRef?
    var displaySleepAssertion: IOPMAssertionID = IOPMAssertionID(0)

    let queue: DispatchQueue = DispatchQueue(label: "io.mpv.queue")

    @objc init(_ mpvHandle: OpaquePointer) {
        libmpv = LibmpvHelper(mpvHandle, "cocoacb")
        super.init()
        libmpv.createRender()
        libmpv.setRenderControlCallback(self.controlCallback, context: self)
    }

    func preinit(_ vo: UnsafeMutablePointer<vo>) {
        if backendState == .uninitialized {
            // Any 1-time initialization for video rendering here
            backendState = .needsInit
            
            libmpv.observeFlag("ontop")
            libmpv.observeFlag("border")
            libmpv.observeFlag("keepaspect-window")
            libmpv.observeString("macos-title-bar-style")
            libmpv.observeString("macos-title-bar-appearance")
            libmpv.observeString("macos-title-bar-material")
            libmpv.observeString("macos-title-bar-color")

            view = EventsView(cocoaCB: self)
        }
        if backendState == .needsInit {
            mpv = MPVHelper(vo, "cocoacb")
            layer = VideoLayer(cocoaCB: self)
            timer =  PreciseTimer(common: self)

            view?.layer = layer
            view?.wantsLayer = true
            view?.layerContentsPlacement = .scaleProportionallyToFit

            startDisplayLink(vo)
            if mpv!.glOpts.gamma_auto != 0 {
                initLightSensor()
            }
            addDisplayReconfigureObserver()
        }
    }

    // This must be idempotent as it can potentially be called multiple times.
    func uninit(unregister: Bool = false) {
        // Note: If the VO uninit was called first, then VO might already be terminated and
        // vo context points to already free'd memory
        print("UNINIT CALLED")
        // This may be called either by voctrl uninit or by ourselves on global quit
        // In the latter case VO is still active
        mpv = nil
        layer?.uninit(unregister)
        layer = nil

        window?.orderOut(nil)
        window?.close()
        timer?.terminate()
        timer = nil
        stopDisplaylink()
        uninitLightSensor()
        removeDisplayReconfigureObserver()
    }

    func reconfig(_ vo: UnsafeMutablePointer<vo>) {
        mpv?.reconfig(vo)
        if backendState == .needsInit {
            DispatchQueue.main.sync { self.initBackend() }
        } else {
            DispatchQueue.main.async {
                self.updateWindowSize()
                self.layer?.update(force: true)
            }
        }
    }

    func initBackend() {
        NSApp.setActivationPolicy(.regular)
        setAppIcon()

        guard let view = self.view else {
            libmpv.sendError("Something went wrong, no View was initialized")
            exit(1)
        }
        guard let targetScreen = getScreenBy(id: Int(mpv!.opts.screen_id)) ?? NSScreen.main else {
            libmpv.sendError("Something went wrong, no Screen was found")
            exit(1)
        }

        let wr = getWindowGeometry(forScreen: targetScreen)
        window = Window(contentRect: wr, screen: targetScreen, view: view, cocoaCB: self)
        guard let window = self.window else {
            libmpv.sendError("Something went wrong, no Window was initialized")
            exit(1)
        }

        updateICCProfile()
        window.setOnTop(Bool(mpv!.opts.ontop), Int(mpv!.opts.ontop_level))
        window.keepAspect = Bool(mpv!.opts.keepaspect_window)
        window.title = title
        window.border = Bool(mpv!.opts.border)

        titleBar = TitleBar(frame: wr, window: window, cocoaCB: self)

        window.isRestorable = false
        window.isReleasedWhenClosed = false
        window.makeMain()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        if Bool(mpv!.opts.fullscreen) {
            DispatchQueue.main.async {
                self.window?.toggleFullScreen(nil)
            }
        } else {
            window.isMovableByWindowBackground = true
        }

        backendState = .initialized
    }

    func updateWindowSize() {
        guard let targetScreen = getScreenBy(id: Int(mpv!.opts.screen_id)) ?? NSScreen.main else {
            libmpv.sendWarning("Couldn't update Window size, no Screen available")
            return
        }

        let wr = getWindowGeometry(forScreen: targetScreen)
        if !(window?.isVisible ?? false) &&
           !(window?.isMiniaturized ?? false) &&
           !NSApp.isHidden
        {
            window?.makeKeyAndOrderFront(nil)
        }
        layer?.atomicDrawingStart()
        window?.updateSize(wr.size)
    }

    func setAppIcon() {
        if let app = NSApp as? Application {
            NSApp.applicationIconImage = app.getMPVIcon()
        }
    }

    let linkCallback: CVDisplayLinkOutputCallback = {
                    (displayLink: CVDisplayLink,
                           inNow: UnsafePointer<CVTimeStamp>,
                    inOutputTime: UnsafePointer<CVTimeStamp>,
                         flagsIn: CVOptionFlags,
                        flagsOut: UnsafeMutablePointer<CVOptionFlags>,
              displayLinkContext: UnsafeMutableRawPointer?) -> CVReturn in
        let ccb = unsafeBitCast(displayLinkContext, to: CocoaCB.self)
        return ccb.displayLinkCallback(displayLink, inNow, inOutputTime, flagsIn, flagsOut)
    }

    func displayLinkCallback(_ displayLink: CVDisplayLink,
                                   _ inNow: UnsafePointer<CVTimeStamp>,
                            _ inOutputTime: UnsafePointer<CVTimeStamp>,
                                 _ flagsIn: CVOptionFlags,
                                _ flagsOut: UnsafeMutablePointer<CVOptionFlags>) -> CVReturn
    {
        // For reasons I don't understand, unblocking the render flip at inNow (0.5 into vsync)
        // results in _lower_ frame timings. But if you do so, then there are also lots of skipped frames.
        // If you unblock at exactly vsync, then there aren't any skipped frames but page timing is high.
        // So we compromise and unblock at 0.25 vsync, which seems to give best of both worlds?
        // To be honest it seems like the double-buffering implemented by caopengllayer seems to
        // be misaligned with the vsync timings somehow. So things only get unblocked like 0.2-in or something,
        // but if you wait until 0.5 then the compositor probably fires which causes issues.
        // Then again the net result is that addition to gpu timing includes the cpu wait time anyhow (4k us => 25% vsync interval)
        let scheduleTime = UInt64(Double(inOutputTime.pointee.hostTime) * 125.0 / 3.0 / 1e3)

        let vsyncInterval = Double(inOutputTime.pointee.videoRefreshPeriod)/Double(inOutputTime.pointee.videoTimeScale)

        timer?.scheduleAt(time: inOutputTime.pointee.hostTime /*+ UInt64(0.25 * vsyncInterval * 1e9 * 3.0/125.0)*/ , closure: {
            self.libmpv.reportRenderFlip(time: scheduleTime)
        })
        return kCVReturnSuccess
    }

    func startDisplayLink(_ vo: UnsafeMutablePointer<vo>) {
        CVDisplayLinkCreateWithActiveCGDisplays(&link)

        guard let screen = getScreenBy(id: Int(mpv!.opts.screen_id)) ?? NSScreen.main,
              let link = self.link else
        {
            libmpv.sendWarning("Couldn't start DisplayLink, no Screen or DisplayLink available")
            return
        }

        CVDisplayLinkSetCurrentCGDisplay(link, screen.displayID)
        CVDisplayLinkSetOutputCallback(link, linkCallback, MPVHelper.bridge(obj: self))
        CVDisplayLinkStart(link)
        timer?.updatePolicy(periodSeconds: 1 / currentFps())
    }

    func stopDisplaylink() {
        if let link = self.link, CVDisplayLinkIsRunning(link) {
            CVDisplayLinkStop(link)
        }
    }

    func updateDisplaylink() {
        guard let screen = window?.screen, let link = self.link else {
            libmpv.sendWarning("Couldn't update DisplayLink, no Screen or DisplayLink available")
            return
        }

        CVDisplayLinkSetCurrentCGDisplay(link, screen.displayID)
        queue.asyncAfter(deadline: DispatchTime.now() + 0.1) {
            self.flagEvents(VO_EVENT_DISPLAY_STATE)
        }
    }

    func currentFps() -> Double {
        if let link = self.link {
            var actualFps = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link)
            let nominalData = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link)

            if (nominalData.flags & Int32(CVTimeFlags.isIndefinite.rawValue)) < 1 {
                let nominalFps = Double(nominalData.timeScale) / Double(nominalData.timeValue)

                if actualFps > 0 {
                    actualFps = 1/actualFps
                }

                if fabs(actualFps - nominalFps) > 0.1 {
                    libmpv.sendVerbose("Falling back to nominal display refresh rate: \(nominalFps)")
                    return nominalFps
                } else {
                    return actualFps
                }
            }
        } else {
            libmpv.sendWarning("No DisplayLink available")
        }

        libmpv.sendWarning("Falling back to standard display refresh rate: 60Hz")
        return 60.0
    }

    func enableDisplaySleep() {
        IOPMAssertionRelease(displaySleepAssertion)
        displaySleepAssertion = IOPMAssertionID(0)
    }

    func disableDisplaySleep() {
        if displaySleepAssertion != IOPMAssertionID(0) { return }
        IOPMAssertionCreateWithName(
            kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
            IOPMAssertionLevel(kIOPMAssertionLevelOn),
            "io.mpv.video_playing_back" as CFString,
            &displaySleepAssertion)
    }

    func updateCusorVisibility() {
        setCursorVisiblility(cursorVisibilityWanted)
    }

    func setCursorVisiblility(_ visible: Bool) {
        let visibility = visible ? true : !(view?.canHideCursor() ?? false)
        if visibility && cursorHidden {
            NSCursor.unhide()
            cursorHidden = false;
        } else if !visibility && !cursorHidden {
            NSCursor.hide()
            cursorHidden = true
        }
    }

    func updateICCProfile() {
        guard let colorSpace = window?.screen?.colorSpace else {
            libmpv.sendWarning("Couldn't update ICC Profile, no color space available")
            return
        }

        libmpv.setRenderICCProfile(colorSpace)
        if #available(macOS 10.11, *) {
            layer?.colorspace = colorSpace.cgColorSpace
        }
    }

    func lmuToLux(_ v: UInt64) -> Int {
        // the polinomial approximation for apple lmu value -> lux was empirically
        // derived by firefox developers (Apple provides no documentation).
        // https://bugzilla.mozilla.org/show_bug.cgi?id=793728
        let power_c4: Double = 1 / pow(10, 27)
        let power_c3: Double = 1 / pow(10, 19)
        let power_c2: Double = 1 / pow(10, 12)
        let power_c1: Double = 1 / pow(10, 5)

        let lum = Double(v)
        let term4: Double = -3.0 * power_c4 * pow(lum, 4.0)
        let term3: Double = 2.6 * power_c3 * pow(lum, 3.0)
        let term2: Double = -3.4 * power_c2 * pow(lum, 2.0)
        let term1: Double = 3.9 * power_c1 * lum

        let lux = Int(ceil(term4 + term3 + term2 + term1 - 0.19))
        return lux > 0 ? lux : 0
    }

    var lightSensorCallback: IOServiceInterestCallback = { (ctx, service, messageType, messageArgument) -> Void in
        let ccb = unsafeBitCast(ctx, to: CocoaCB.self)

        var outputs: UInt32 = 2
        var values: [UInt64] = [0, 0]

        var kr = IOConnectCallMethod(ccb.lightSensor, 0, nil, 0, nil, 0, &values, &outputs, nil, nil)
        if kr == KERN_SUCCESS {
            var mean = (values[0] + values[1]) / 2
            if ccb.lastLmu != mean {
                ccb.lastLmu = mean
                ccb.layer?.updateLux = ccb.lmuToLux(ccb.lastLmu)
            }
        }
    }

    func initLightSensor() {
        let srv = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("AppleLMUController"))
        if srv == IO_OBJECT_NULL {
            libmpv.sendVerbose("Can't find an ambient light sensor")
            return
        }

        lightSensorIOPort = IONotificationPortCreate(kIOMasterPortDefault)
        IONotificationPortSetDispatchQueue(lightSensorIOPort, queue)
        var n = io_object_t()
        IOServiceAddInterestNotification(lightSensorIOPort, srv, kIOGeneralInterest, lightSensorCallback, MPVHelper.bridge(obj: self), &n)
        let kr = IOServiceOpen(srv, mach_task_self_, 0, &lightSensor)
        IOObjectRelease(srv)

        if kr != KERN_SUCCESS {
            libmpv.sendVerbose("Can't start ambient light sensor connection")
            return
        }
        lightSensorCallback(MPVHelper.bridge(obj: self), 0, 0, nil)
    }

    func uninitLightSensor() {
        if lightSensorIOPort != nil {
            IONotificationPortDestroy(lightSensorIOPort)
            IOObjectRelease(lightSensor)
            lightSensorIOPort = nil
        }
    }

    var reconfigureCallback: CGDisplayReconfigurationCallBack = { (display, flags, userInfo) in
        if flags.contains(.setModeFlag) {
            let ccb = unsafeBitCast(userInfo, to: CocoaCB.self)
            let displayID = ccb.window?.screen?.displayID ?? display

            if displayID == display {
                ccb.libmpv.sendVerbose("Detected display mode change, updating screen refresh rate");
                ccb.flagEvents(VO_EVENT_DISPLAY_STATE)
            }
        }
    }

    func addDisplayReconfigureObserver() {
        CGDisplayRegisterReconfigurationCallback(reconfigureCallback, MPVHelper.bridge(obj: self))
    }

    func removeDisplayReconfigureObserver() {
        CGDisplayRemoveReconfigurationCallback(reconfigureCallback, MPVHelper.bridge(obj: self))
    }

    func getTargetScreen(forFullscreen fs: Bool) -> NSScreen? {
        let screenID = fs ? (mpv?.opts.fsscreen_id ?? 0) : (mpv?.opts.screen_id ?? 0)
        return getScreenBy(id: Int(screenID))
    }

    func getScreenBy(id screenID: Int) -> NSScreen? {
        if screenID >= NSScreen.screens.count {
            libmpv.sendInfo("Screen ID \(screenID) does not exist, falling back to current device")
            return nil
        } else if screenID < 0 {
            return nil
        }
        return NSScreen.screens[screenID]
    }

    func getWindowGeometry(forScreen targetScreen: NSScreen) -> NSRect {
        let r = targetScreen.convertRectToBacking(targetScreen.frame)
        var screenRC: mp_rect = mp_rect(x0: Int32(0),
                                        y0: Int32(0),
                                        x1: Int32(r.size.width),
                                        y1: Int32(r.size.height))

        var geo: vo_win_geometry = vo_win_geometry()
        vo_calc_window_geometry2(mpv!.vo, &screenRC, Double(targetScreen.backingScaleFactor), &geo)

        // flip y coordinates
        geo.win.y1 = Int32(r.size.height) - geo.win.y1
        geo.win.y0 = Int32(r.size.height) - geo.win.y0

        let wr = NSMakeRect(CGFloat(geo.win.x0), CGFloat(geo.win.y1),
                            CGFloat(geo.win.x1 - geo.win.x0),
                            CGFloat(geo.win.y0 - geo.win.y1))
        return targetScreen.convertRectFromBacking(wr)
    }

    func flagEvents(_ ev: Int) {
        eventsLock.lock()
        events |= ev
        eventsLock.unlock()

        if mpv != nil {
            mpv!.wakeup_vo()
        } else {
            libmpv.sendWarning("vo nil in flagEvents")
        }
    }

    func checkEvents() -> Int {
        eventsLock.lock()
        let ev = events
        events = 0
        eventsLock.unlock()
        return ev
    }

    var controlCallback: mp_render_cb_control_fn = { ( vo, ctx, events, request, data ) -> Int32 in
        let ccb = unsafeBitCast(ctx, to: CocoaCB.self)
        guard let opts: UnsafeMutablePointer<mp_vo_opts> = vo?.pointee.opts else {
            ccb.libmpv.sendWarning("Nil vo or opts in Control Callback")
            return VO_FALSE
        }

        switch mp_voctrl(request) {
        case VOCTRL_CHECK_EVENTS:
            if let ev = events {
                ev.pointee = Int32(ccb.checkEvents())
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_FULLSCREEN:
            DispatchQueue.main.async {
                ccb.window?.toggleFullScreen(nil)
            }
            return VO_TRUE
        case VOCTRL_GET_FULLSCREEN:
            if let fsData = data?.assumingMemoryBound(to: Int32.self) {
                fsData.pointee = (ccb.window?.isInFullscreen ?? false) ? 1 : 0
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_GET_DISPLAY_FPS:
            if let fps = data?.assumingMemoryBound(to: CDouble.self) {
                fps.pointee = ccb.currentFps()
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_GET_HIDPI_SCALE:
            if let scaleFactor = data?.assumingMemoryBound(to: CDouble.self) {
                let factor = ccb.window?.backingScaleFactor ??
                             ccb.getTargetScreen(forFullscreen: false)?.backingScaleFactor ??
                             NSScreen.main?.backingScaleFactor ?? 1.0
                scaleFactor.pointee = Double(factor)
                return VO_TRUE;
            }
            return VO_FALSE
        case VOCTRL_RESTORE_SCREENSAVER:
            ccb.enableDisplaySleep()
            return VO_TRUE
        case VOCTRL_KILL_SCREENSAVER:
            ccb.disableDisplaySleep()
            return VO_TRUE
        case VOCTRL_SET_CURSOR_VISIBILITY:
            if let cursorVisibility = data?.assumingMemoryBound(to: CBool.self) {
                ccb.cursorVisibilityWanted = cursorVisibility.pointee
                DispatchQueue.main.async {
                    ccb.setCursorVisiblility(ccb.cursorVisibilityWanted)
                }
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_GET_UNFS_WINDOW_SIZE:
            let sizeData = data?.assumingMemoryBound(to: Int32.self)
            let size = UnsafeMutableBufferPointer(start: sizeData, count: 2)
            var rect = ccb.window?.unfsContentFrame ?? NSRect(x: 0, y: 0, width: 1280, height: 720)
            if let screen = ccb.window?.currentScreen, !Bool(opts.pointee.hidpi_window_scale) {
                rect = screen.convertRectToBacking(rect)
            }
            size[0] = Int32(rect.size.width)
            size[1] = Int32(rect.size.height)
            return VO_TRUE
        case VOCTRL_SET_UNFS_WINDOW_SIZE:
            if let sizeData = data?.assumingMemoryBound(to: Int32.self) {
                let size = UnsafeBufferPointer(start: sizeData, count: 2)
                var rect = NSMakeRect(0, 0, CGFloat(size[0]), CGFloat(size[1]))
                DispatchQueue.main.sync {
                    if let screen = ccb.window?.currentScreen, !Bool(opts.pointee.hidpi_window_scale) {
                        rect = screen.convertRectFromBacking(rect)
                    }
                    ccb.window?.updateSize(rect.size, reconfig: false)
                }
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_GET_WIN_STATE:
            if let minimized = data?.assumingMemoryBound(to: Int32.self) {
                minimized.pointee = ccb.window?.isMiniaturized ?? false ?
                    VO_WIN_STATE_MINIMIZED : Int32(0)
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_GET_DISPLAY_NAMES:
            if let dnames = data?.assumingMemoryBound(to: UnsafeMutablePointer<UnsafeMutablePointer<Int8>?>?.self) {
                var array: UnsafeMutablePointer<UnsafeMutablePointer<Int8>?>? = nil
                var count: Int32 = 0
                let screen = ccb.window != nil ? ccb.window?.screen :
                                                 ccb.getTargetScreen(forFullscreen: false) ??
                                                 NSScreen.main
                let displayName = screen?.displayName ?? "Unknown"

                SWIFT_TARRAY_STRING_APPEND(nil, &array, &count, ta_xstrdup(nil, displayName))
                SWIFT_TARRAY_STRING_APPEND(nil, &array, &count, nil)
                dnames.pointee = array
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_UPDATE_WINDOW_TITLE:
            if let titleData = data?.assumingMemoryBound(to: Int8.self) {
                let title = String(cString: titleData)
                DispatchQueue.main.async {
                    ccb.title = title
                }
                return VO_TRUE
            }
            return VO_FALSE
        case VOCTRL_PREINIT:
            DispatchQueue.main.sync { ccb.preinit(vo!) }
            return VO_TRUE
        case VOCTRL_UNINIT:
            // Avoid a deadlock: if global quit is called first (from main thread), it calls ccb.uninit() which calls
            // libmpv uninit, which sends voctrl_uninit (from VO thread) which should not again block on main thread.
            // Luckily we can avoid a lock here since mp_dispatch_append impliictly gives us the happens-before relation
            // and guarantees that when the VO thread comes here the write to ccb.mpv is visible.
            if ccb.mpv != nil {
                DispatchQueue.main.sync { ccb.mpv = nil }
            }
            // Past this point nothing should make use of ccb.mpv
            DispatchQueue.main.async { ccb.uninit(); ccb.backendState = .needsInit }
            return VO_TRUE
        case VOCTRL_RECONFIG:
            ccb.reconfig(vo!)
            return VO_TRUE
        default:
            return VO_NOTIMPL
        }
    }

    func shutdown(_ destroy: Bool = false) {
        isShuttingDown = window?.isAnimating ?? false ||
                         window?.isInFullscreen ?? false && Bool(mpv?.opts.native_fs ?? 1)
        if window?.isInFullscreen ?? false && !(window?.isAnimating ?? false) {
            window?.close()
        }
        if isShuttingDown { return }
        uninit(unregister: true)
        libmpv.freeRenderer()
        setCursorVisiblility(true)
        libmpv.deinitMPV(destroy)
    }

    func checkShutdown() {
        if isShuttingDown {
            shutdown(true)
        }
    }

    // This is called from the main thread, so it's already serialized against other calls to uninit()
    @objc func processEvent(_ event: UnsafePointer<mpv_event>) {
        switch event.pointee.event_id {
        case MPV_EVENT_SHUTDOWN:
            shutdown()
        case MPV_EVENT_PROPERTY_CHANGE:
            if backendState == .initialized {
                handlePropertyChange(event)
            }
        default:
            break
        }
    }

    func handlePropertyChange(_ event: UnsafePointer<mpv_event>) {
        let pData = OpaquePointer(event.pointee.data)
        guard let property = UnsafePointer<mpv_event_property>(pData)?.pointee else {
            return
        }

        switch String(cString: property.name) {
        case "border":
            if let data = LibmpvHelper.mpvFlagToBool(property.data) {
                window?.border = data
            }
        case "ontop":
            if let data = LibmpvHelper.mpvFlagToBool(property.data) {
                window?.setOnTop(data, Int(mpv?.opts.ontop_level ?? -1))
            }
        case "keepaspect-window":
            if let data = LibmpvHelper.mpvFlagToBool(property.data) {
                window?.keepAspect = data
            }
        case "macos-title-bar-appearance":
            if let data = LibmpvHelper.mpvStringArrayToString(property.data) {
                titleBar?.set(appearance: data)
            }
        case "macos-title-bar-material":
            if let data = LibmpvHelper.mpvStringArrayToString(property.data) {
                titleBar?.set(material: data)
            }
        case "macos-title-bar-color":
            if let data = LibmpvHelper.mpvStringArrayToString(property.data) {
                titleBar?.set(color: data)
            }
        default:
            break
        }
    }
}