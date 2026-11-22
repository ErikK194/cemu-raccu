package info.cemu.cemu.emulation.input

import android.view.KeyEvent
import android.view.MotionEvent
import info.cemu.cemu.common.android.inputevent.isFromPhysicalController
import info.cemu.cemu.nativeinterface.NativeInput

object InputHandler {
    fun onKeyEvent(event: KeyEvent): Boolean {
        if (!event.isFromPhysicalController()) {
            return false
        }

        val device = event.device

        NativeInput.onControllerKey(
            device.descriptor,
            event.keyCode,
            event.action == KeyEvent.ACTION_DOWN
        )

        return true
    }

    fun onMotionEvent(event: MotionEvent): Boolean {
        if (!event.isFromPhysicalController()) {
            return false
        }

        val device = event.device
        val actionPointerIndex = event.actionIndex
        // built-in pads of handhelds (e.g. Retroid Pocket 5) can report the analog L2/R2 as BRAKE/GAS, some as both
        // BRAKE/GAS and LTRIGGER/RTRIGGER: forward the stronger value of each pair as LTRIGGER/RTRIGGER
        var leftTrigger: Float? = null
        var rightTrigger: Float? = null
        for (motionRange in device.motionRanges) {
            val axisValue = event.getAxisValue(motionRange.axis, actionPointerIndex)
            when (val axis = motionRange.axis) {
                MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE ->
                    leftTrigger = maxOf(leftTrigger ?: 0f, axisValue)

                MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS ->
                    rightTrigger = maxOf(rightTrigger ?: 0f, axisValue)

                else -> NativeInput.onControllerAxis(device.descriptor, axis, axisValue)
            }
        }
        leftTrigger?.let { NativeInput.onControllerAxis(device.descriptor, MotionEvent.AXIS_LTRIGGER, it) }
        rightTrigger?.let { NativeInput.onControllerAxis(device.descriptor, MotionEvent.AXIS_RTRIGGER, it) }

        return true
    }
}
