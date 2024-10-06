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

class MPVHelper {

    var vo: UnsafeMutablePointer<vo>
    private var input: OpaquePointer { get { return vo.pointee.input_ctx } }

    var opts: mp_vo_opts
    var glOpts: gl_video_opts
    var macOpts: macos_opts
    var log: LogHelper

    init(_ vo: UnsafeMutablePointer<vo>, _ name: String) {
        self.vo = vo
        self.opts = vo.pointee.opts.pointee

        let newlog = mp_log_new(vo, vo.pointee.log, name)
        self.log = LogHelper(newlog)

        guard let app = NSApp as? Application,
              let macOptPtr = mp_get_config_group(vo,
                                            vo.pointee.global,
                                            macos_conf_ptr) else
        {
            log.sendError("macOS config group couldn't be retrieved'")
            exit(1)
        }
        self.macOpts = UnsafeMutablePointer<macos_opts>(OpaquePointer(macOptPtr)).pointee
        guard let glOptPtr = mp_get_config_group(vo,
                                            vo.pointee.global,
                                            gl_video_conf_ptr) else
        {
            log.sendError("GL config group couldn't be retrieved'")
            exit(1)
        }
        self.glOpts = UnsafeMutablePointer<gl_video_opts>(OpaquePointer(glOptPtr)).pointee
    }

    func reconfig(_ vo: UnsafeMutablePointer<vo>) {
        opts = vo.pointee.opts.pointee
    }

    func wakeup_vo() {
        vo_wakeup(vo)
    }

    func canBeDraggedAt(_ pos: NSPoint) -> Bool {
        let canDrag = !mp_input_test_dragging(input, Int32(pos.x), Int32(pos.y))
        return canDrag
    }

    func mouseEnabled() -> Bool {
        return mp_input_mouse_enabled(input)
    }

    func setMousePosition(_ pos: NSPoint) {
        mp_input_set_mouse_pos(input, Int32(pos.x), Int32(pos.y))
    }

    func putAxis(_ mpkey: Int32, delta: Double) {
        mp_input_put_wheel(input, mpkey, delta)
    }

    func command(_ cmd: String) {
        let cCmd = UnsafePointer<Int8>(strdup(cmd))
        let mpvCmd = mp_input_parse_cmd(input, bstrof0(cCmd), "")
        mp_input_queue_cmd(input, mpvCmd)
        free(UnsafeMutablePointer(mutating: cCmd))
    }

    // (__bridge void*)
    class func bridge<T: AnyObject>(obj: T) -> UnsafeMutableRawPointer {
        return UnsafeMutableRawPointer(Unmanaged.passUnretained(obj).toOpaque())
    }

    // (__bridge T*)
    class func bridge<T: AnyObject>(ptr: UnsafeRawPointer) -> T {
        return Unmanaged<T>.fromOpaque(ptr).takeUnretainedValue()
    }
}