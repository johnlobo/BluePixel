import AVFoundation
import CPCCore

/// Pulls samples from the emulator's audio ring buffer.
final class AudioOutput {
    private let engine = AVAudioEngine()
    private var node: AVAudioSourceNode?
    private let cpc: UnsafeMutablePointer<CPC>
    var muted = false
    private var scratch = [Float](repeating: 0, count: 8192 * 2)

    init(cpc: UnsafeMutablePointer<CPC>, sampleRate: Double = 44100) {
        self.cpc = cpc
        let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sampleRate, channels: 2, interleaved: false)!
        node = AVAudioSourceNode(format: format) { [weak self] _, _, frameCount, audioBufferList -> OSStatus in
            guard let self = self else { return noErr }
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            let n = Int(frameCount)
            guard abl.count >= 2, let l = abl[0].mData?.assumingMemoryBound(to: Float.self), let r = abl[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            var got = 0
            if n <= 8192 {
                got = Int(self.scratch.withUnsafeMutableBufferPointer { cpc_audio_read(self.cpc, $0.baseAddress, Int32(n)) })
                if self.muted { got = 0 }
                for i in 0..<got { l[i] = self.scratch[i * 2]; r[i] = self.scratch[i * 2 + 1] }
            }
            for i in got..<n { l[i] = 0; r[i] = 0 }
            return noErr
        }
        engine.attach(node!)
        engine.connect(node!, to: engine.mainMixerNode, format: format)
    }

    func start() { try? engine.start() }
    func stop() { engine.stop() }
}
