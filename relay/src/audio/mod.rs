//! Audio pipeline traits + implementations.
//!
//!   AudioSource (produces interleaved f32)  ->  StreamEncoder (bytes on the wire)
//!
//! v1 wire format is PCM s16le 44.1kHz stereo (~1.41 Mbps/listener). The
//! `StreamEncoder` trait is the seam for a future Opus backend — adding it does
//! NOT change the client protocol, only adds a new `fmt` value (ARCHITECTURE.md).

pub mod tone;

#[cfg(feature = "librespot")]
pub mod librespot_source;

pub const SAMPLE_RATE: u32 = 44_100;
pub const CHANNELS: u16 = 2;

/// A pull-based source of interleaved f32 PCM (L, R, L, R, ...).
pub trait AudioSource: Send {
    /// Fill `out` with interleaved f32 samples; return how many were written.
    /// Returning 0 means "no data yet" (silence/underrun) — caller paces.
    fn read(&mut self, out: &mut [f32]) -> usize;
    fn sample_rate(&self) -> u32 {
        SAMPLE_RATE
    }
    fn channels(&self) -> u16 {
        CHANNELS
    }
    /// Seek to a position in milliseconds (sources that support it override).
    fn seek(&mut self, _position_ms: u64) {}
}

/// Encodes f32 samples to wire bytes. One impl per `fmt`.
pub trait StreamEncoder: Send {
    fn format_name(&self) -> &'static str;
    fn content_type(&self) -> &'static str;
    /// Append the encoding of `samples` (interleaved f32 @ SAMPLE_RATE) to `out`.
    fn encode(&mut self, samples: &[f32], out: &mut Vec<u8>);
    /// Output sample rate advertised to the client (may differ from input if the
    /// encoder decimates, e.g. ADPCM at half rate). Defaults to input rate.
    fn out_sample_rate(&self) -> u32 {
        SAMPLE_RATE
    }
}

/// PCM signed 16-bit little-endian. The v1 default.
pub struct PcmS16LeEncoder;

impl StreamEncoder for PcmS16LeEncoder {
    fn format_name(&self) -> &'static str {
        "pcm_s16le"
    }
    fn content_type(&self) -> &'static str {
        "application/octet-stream"
    }
    fn encode(&mut self, samples: &[f32], out: &mut Vec<u8>) {
        out.reserve(samples.len() * 2);
        for &s in samples {
            let clamped = s.clamp(-1.0, 1.0);
            let v = (clamped * i16::MAX as f32) as i16;
            out.extend_from_slice(&v.to_le_bytes());
        }
    }
}

/// IMA ADPCM encoder, ~4:1, decimated to half rate (22050 Hz) for a ~22 KB/s
/// stereo stream — fits low-bandwidth clients (Wii U Wi-Fi caps ~50 KB/s, raw
/// PCM needs 176 KB/s). Continuous per-channel state over a reliable (TCP)
/// stream; the client decoder mirrors it from sample 0.
pub const ADPCM_SAMPLE_RATE: u32 = SAMPLE_RATE / 2; // 22050

const STEP_TABLE: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
];
const INDEX_TABLE: [i32; 16] =
    [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

#[derive(Default, Clone, Copy)]
struct AdpcmChannel {
    predictor: i32,
    index: i32,
}

impl AdpcmChannel {
    fn encode(&mut self, sample: i16) -> u8 {
        let mut step = STEP_TABLE[self.index as usize];
        let mut diff = sample as i32 - self.predictor;
        let mut code = 0u8;
        if diff < 0 {
            code = 8;
            diff = -diff;
        }
        let mut vpdiff = step >> 3;
        if diff >= step {
            code |= 4;
            diff -= step;
            vpdiff += step;
        }
        step >>= 1;
        if diff >= step {
            code |= 2;
            diff -= step;
            vpdiff += step;
        }
        step >>= 1;
        if diff >= step {
            code |= 1;
            vpdiff += step;
        }
        if code & 8 != 0 {
            self.predictor -= vpdiff;
        } else {
            self.predictor += vpdiff;
        }
        self.predictor = self.predictor.clamp(-32768, 32767);
        self.index = (self.index + INDEX_TABLE[code as usize]).clamp(0, 88);
        code
    }
}

pub struct ImaAdpcmEncoder {
    ch: [AdpcmChannel; 2],
    decim_phase: u8,      // keep every other input frame (44100 -> 22050)
    pending: Option<u8>,  // low nibble awaiting its high nibble pair
}

impl ImaAdpcmEncoder {
    pub fn new() -> Self {
        ImaAdpcmEncoder {
            ch: [AdpcmChannel::default(); 2],
            decim_phase: 0,
            pending: None,
        }
    }
    #[inline]
    fn push_nibble(&mut self, n: u8, out: &mut Vec<u8>) {
        match self.pending.take() {
            None => self.pending = Some(n & 0x0f),
            Some(low) => out.push(low | (n << 4)),
        }
    }
}

impl StreamEncoder for ImaAdpcmEncoder {
    fn format_name(&self) -> &'static str {
        "adpcm_ima"
    }
    fn content_type(&self) -> &'static str {
        "application/octet-stream"
    }
    fn out_sample_rate(&self) -> u32 {
        ADPCM_SAMPLE_RATE
    }
    fn encode(&mut self, samples: &[f32], out: &mut Vec<u8>) {
        let frames = samples.len() / CHANNELS as usize;
        for f in 0..frames {
            // decimate by 2: keep every other stereo frame
            if self.decim_phase == 0 {
                let l = (samples[f * 2].clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
                let r = (samples[f * 2 + 1].clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
                let nl = self.ch[0].encode(l);
                let nr = self.ch[1].encode(r);
                self.push_nibble(nl, out); // L then R, alternating channels
                self.push_nibble(nr, out);
            }
            self.decim_phase ^= 1;
        }
    }
}

/// Pick an encoder for a requested `fmt` (defaults to pcm_s16le).
pub fn encoder_for(fmt: &str) -> Option<Box<dyn StreamEncoder>> {
    match fmt {
        "pcm_s16le" | "" => Some(Box::new(PcmS16LeEncoder)),
        "adpcm_ima" => Some(Box::new(ImaAdpcmEncoder::new())),
        _ => None,
    }
}
