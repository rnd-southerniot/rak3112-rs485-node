// ChirpStack v4 codec for rak3112-rs485-node — ADR-005 compact versioned binary payload.
// Matches firmware/components/payload/payload.c. Paste into the ChirpStack Device Profile
// "Codec" tab (JavaScript functions). Big-endian throughout.
//
// Header (3 bytes): [0]=schema version, [1]=device type, [2]=flags.
//   device: 0x01 MFM384, 0x02 RS-FSJT-N01.  flags bit0=simulated, bit1=stale.
// MFM384 body (16 bytes): V1N,V2N,V3N (u16 ÷10 V), TotalkW (i16 ÷10), TotalkWh (u32 ÷100),
//   Freq (u16 ÷100 Hz), AvgPF (i16 ÷1000).
// RS-FSJT body (2 bytes): wind (u16 ÷100 m/s).

function decodeUplink(input) {
  var b = input.bytes;
  if (!b || b.length < 3) {
    return { errors: ["payload too short (" + (b ? b.length : 0) + " bytes)"] };
  }

  var u16 = function (i) { return (b[i] << 8) | b[i + 1]; };
  var i16 = function (i) { var v = u16(i); return v > 0x7fff ? v - 0x10000 : v; };
  var u32 = function (i) {
    return ((b[i] * 0x1000000) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3]);
  };

  var version = b[0];
  var device = b[1];
  var flags = b[2];
  var data = {
    schema_version: version,
    simulated: (flags & 0x01) !== 0,
    stale: (flags & 0x02) !== 0,
  };

  if (device === 0x01) {
    if (b.length < 19) return { errors: ["MFM384 payload too short"] };
    data.device = "MFM384";
    data.v1n_V = u16(3) / 10;
    data.v2n_V = u16(5) / 10;
    data.v3n_V = u16(7) / 10;
    data.total_kW = i16(9) / 10;
    data.total_kWh = u32(11) / 100;
    data.freq_Hz = u16(15) / 100;
    data.avg_pf = i16(17) / 1000;
  } else if (device === 0x02) {
    if (b.length < 5) return { errors: ["RS-FSJT payload too short"] };
    data.device = "RS-FSJT-N01";
    data.wind_mps = u16(3) / 100;
  } else {
    return { errors: ["unknown device type 0x" + device.toString(16)] };
  }

  return { data: data };
}

// Downlink not used by this node; provided so the profile validates.
function encodeDownlink(input) {
  return { bytes: [] };
}
