//! Decode decrypted Deezer MP3/FLAC bytes to interleaved f32 stereo PCM via
//! symphonia. Deezer serves 44.1kHz; mono is upmixed to stereo. (No resampling:
//! the ADPCM encoder expects 44.1k and decimates to 22.05k.)

use symphonia::core::audio::{SampleBuffer, SignalSpec};
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

/// Returns (interleaved_stereo_f32, sample_rate).
pub fn decode_to_pcm(data: Vec<u8>) -> anyhow::Result<(Vec<f32>, u32)> {
    let mss = MediaSourceStream::new(Box::new(std::io::Cursor::new(data)), Default::default());
    let probed = symphonia::default::get_probe().format(
        &Hint::new(),
        mss,
        &FormatOptions::default(),
        &MetadataOptions::default(),
    )?;
    let mut format = probed.format;
    let track = format
        .default_track()
        .ok_or_else(|| anyhow::anyhow!("no default track"))?;
    let track_id = track.id;
    let mut decoder =
        symphonia::default::get_codecs().make(&track.codec_params, &DecoderOptions::default())?;

    let mut out: Vec<f32> = Vec::new();
    let mut rate = 44_100u32;
    loop {
        let packet = match format.next_packet() {
            Ok(p) => p,
            Err(_) => break, // end of stream
        };
        if packet.track_id() != track_id {
            continue;
        }
        match decoder.decode(&packet) {
            Ok(decoded) => {
                let spec: SignalSpec = *decoded.spec();
                rate = spec.rate;
                let channels = spec.channels.count();
                let mut sb = SampleBuffer::<f32>::new(decoded.capacity() as u64, spec);
                sb.copy_interleaved_ref(decoded);
                let s = sb.samples();
                if channels == 2 {
                    out.extend_from_slice(s);
                } else if channels == 1 {
                    for &m in s {
                        out.push(m);
                        out.push(m); // upmix mono -> stereo
                    }
                } else {
                    // take first two channels of each frame
                    for frame in s.chunks(channels) {
                        out.push(frame[0]);
                        out.push(*frame.get(1).unwrap_or(&frame[0]));
                    }
                }
            }
            Err(symphonia::core::errors::Error::DecodeError(_)) => continue,
            Err(_) => break,
        }
    }
    Ok((out, rate))
}
