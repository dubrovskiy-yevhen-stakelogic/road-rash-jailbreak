#!/usr/bin/env python3
"""Scout probe: swanstation / DuckStation-lineage PS1 save states (version 55).

Throwaway RE probe -- not production code.  The container layout below was derived by
reading the serialisation source of the emulator that wrote our files (swanstation,
GPL-3.0: format facts only, no code copied) and then re-verified against the bytes of our
own save states.
Every structural claim is re-checked at parse time; the parser refuses a file whose
component boundaries do not land exactly on the next marker.

Container
---------
    file := SAVE_STATE_HEADER(216) media_filename[len] data[data_uncompressed_size]

    header (little endian, #pragma pack(4)):
      0x00 u32 magic            'DUCC' = 0x43435544
      0x04 u32 version          55 in our files
      0x08 char[128] title
      0x88 char[32]  game_code
      0xA8 u32 media_filename_length
      0xAC u32 offset_to_media_filename
      0xB0 u32 media_subimage_index
      0xB4 u32 unused_offset_to_playlist_filename
      0xB8 u32 screenshot_width / 0xBC height / 0xC0 size / 0xC4 offset
      0xC8 u32 data_compression_type      0 = none (our files)
      0xCC u32 data_compressed_size
      0xD0 u32 data_uncompressed_size
      0xD4 u32 offset_to_data

    The data blob is a flat stream of components separated by string markers.
    A marker is u32 length + that many raw chars (no NUL).  Component order:

      "System" region:u32 frame_number:u32 internal_frame_number:u32
      "CPU"    <CPU block, 6628 bytes, fixed>
      "Bus"    ram_size:u32 5*3*u32 access times, RAM(2MB), BIOS(512KB),
               MEMCTRL 9*u32, ram_size_reg:u32, tty_line_buffer:string
      "DMA" "InterruptController"
      "GPU"    <GPU register block, 160 bytes fixed + FIFO + blit buffer + 16>
      "GPU-VRAM" VRAM(1MB, 1024x512 halfwords)
      "CDROM" "Pad" "Timers"
      "SPU"    <SPU register block> SPU RAM(512KB)   <- RAM is last in the block
      "MDEC" "SIO" "Events" "Overclock" ...

Sub-commands:
    info    <sav>                 header, marker map, component offsets, CPU/GPU summary
    extract <sav> <outdir>        ram.bin vram.bin spuram.bin bios.bin cpu.json gpu.json
    vram-png <sav> <out.png>      whole 1024x512 VRAM as BGR555 -> RGB
    cluts   <sav> <outdir>        CLUT sweep + cross-check against DATA/G_OBJ01.GTP
"""

import json
import os
import struct
import sys
import zlib

SAVE_STATE_MAGIC = 0x43435544          # 'DUCC'
HEADER_SIZE = 216

RAM_2MB_SIZE = 0x200000
BIOS_SIZE = 0x80000
VRAM_WIDTH, VRAM_HEIGHT = 1024, 512
VRAM_SIZE = VRAM_WIDTH * VRAM_HEIGHT * 2
SPU_RAM_SIZE = 512 * 1024

DCACHE_SIZE = 0x400
ICACHE_SIZE = 0x1000
ICACHE_LINES = ICACHE_SIZE // 16
GTE_NUM_REGS = 64                       # 32 data + 32 control

# Bus::DoState: ram_size + 5 x std::array<TickCount,3>
BUS_PRE_RAM = 4 + 5 * 3 * 4             # = 64

GPR_NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
             't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
             's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
             't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']

# CPU::DoState order for the COP0 registers
COP0_NAMES = ['BPC', 'BDA', 'TAR', 'BadVaddr', 'BDAM', 'BPCM',
              'EPC', 'PRID', 'SR', 'CAUSE', 'DCIC']

MARKERS = ['System', 'CPU', 'Bus', 'DMA', 'InterruptController', 'GPU',
           'GPU-VRAM', 'CDROM', 'Pad', 'Timers', 'SPU', 'MDEC', 'SIO',
           'Events', 'Overclock']

CONSOLE_REGION = {0: 'Auto', 1: 'NTSC-J', 2: 'NTSC-U', 3: 'PAL'}
BLITTER_STATE = {0: 'Idle', 1: 'ReadingVRAM', 2: 'WritingVRAM', 3: 'DrawingPolyLine'}
TEXTURE_MODE = {0: 'Palette4Bit', 1: 'Palette8Bit', 2: 'Direct16Bit', 3: 'Reserved_Direct16Bit'}
TRANSPARENCY_MODE = {0: 'HalfBackgroundPlusHalfForeground', 1: 'BackgroundPlusForeground',
                     2: 'BackgroundMinusForeground', 3: 'BackgroundPlusQuarterForeground'}


class ParseError(Exception):
    pass


# ---------------------------------------------------------------------------
# little-endian cursor
# ---------------------------------------------------------------------------

class Cursor(object):
    def __init__(self, data, offset=0):
        self.d = data
        self.o = offset

    def u8(self):
        v = self.d[self.o]
        self.o += 1
        return v

    def b(self):
        return self.u8() != 0

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.o)[0]
        self.o += 2
        return v

    def s16(self):
        v = struct.unpack_from('<h', self.d, self.o)[0]
        self.o += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]
        self.o += 4
        return v

    def s32(self):
        v = struct.unpack_from('<i', self.d, self.o)[0]
        self.o += 4
        return v

    def skip(self, n):
        self.o += n


def marker_bytes(name):
    return struct.pack('<I', len(name)) + name.encode('ascii')


# ---------------------------------------------------------------------------
# container
# ---------------------------------------------------------------------------

class SaveState(object):
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.d = f.read()
        if len(self.d) < HEADER_SIZE:
            raise ParseError('file shorter than a save-state header')

        (self.magic, self.version) = struct.unpack_from('<II', self.d, 0)
        if self.magic != SAVE_STATE_MAGIC:
            raise ParseError('bad magic 0x%08X (expected 0x%08X)' % (self.magic, SAVE_STATE_MAGIC))
        self.title = self.d[8:8 + 128].split(b'\0')[0].decode('latin-1')
        self.game_code = self.d[136:136 + 32].split(b'\0')[0].decode('latin-1')
        (self.media_filename_length, self.offset_to_media_filename,
         self.media_subimage_index, self._unused,
         self.screenshot_width, self.screenshot_height,
         self.screenshot_size, self.offset_to_screenshot,
         self.data_compression_type, self.data_compressed_size,
         self.data_uncompressed_size, self.offset_to_data) = struct.unpack_from('<12I', self.d, 168)

        self.media_filename = self.d[self.offset_to_media_filename:
                                     self.offset_to_media_filename + self.media_filename_length].decode('latin-1')
        if self.data_compression_type != 0:
            raise ParseError('data_compression_type %d is not "none"; this probe only '
                             'handles uncompressed state blobs' % self.data_compression_type)
        self.data_begin = self.offset_to_data
        self.data_end = self.offset_to_data + self.data_uncompressed_size
        if self.data_end > len(self.d):
            raise ParseError('data blob runs past EOF')

        self.marker = self._locate_markers()
        self._layout()

    # -- markers ------------------------------------------------------------
    def _locate_markers(self):
        """Absolute file offsets of each component marker, in stream order.

        Markers must occur exactly once inside the data blob and in the order
        System::DoState writes them; anything else means the layout assumption
        is wrong and we refuse to guess.
        """
        pos = {}
        prev = self.data_begin
        for name in MARKERS:
            pat = marker_bytes(name)
            hits = []
            start = self.data_begin
            while True:
                i = self.d.find(pat, start, self.data_end)
                if i < 0:
                    break
                hits.append(i)
                start = i + 1
            if len(hits) != 1:
                raise ParseError('marker %r found %d times in the data blob (expected 1)'
                                 % (name, len(hits)))
            if hits[0] < prev:
                raise ParseError('marker %r is out of stream order' % name)
            pos[name] = hits[0]
            prev = hits[0]
        return pos

    def _after(self, name):
        return self.marker[name] + 4 + len(name)

    # -- component boundaries ----------------------------------------------
    def _layout(self):
        # System
        c = Cursor(self.d, self._after('System'))
        self.region = c.u32()
        self.frame_number = c.u32()
        self.internal_frame_number = c.u32()
        if c.o != self.marker['CPU']:
            raise ParseError('System block ends at 0x%X, CPU marker at 0x%X'
                             % (c.o, self.marker['CPU']))

        # CPU (fixed size; the end must land exactly on the Bus marker)
        self.cpu_off = self._after('CPU')
        self.cpu = self._parse_cpu(self.cpu_off)
        if self.cpu['_end'] != self.marker['Bus']:
            raise ParseError('CPU block ends at 0x%X, Bus marker at 0x%X'
                             % (self.cpu['_end'], self.marker['Bus']))

        # Bus
        bus = Cursor(self.d, self._after('Bus'))
        self.bus_ram_size = bus.u32()
        if self.bus_ram_size != RAM_2MB_SIZE:
            raise ParseError('Bus ram_size is 0x%X, only 2MB consoles handled' % self.bus_ram_size)
        self.access_times = [[bus.s32() for _ in range(3)] for _ in range(5)]
        self.ram_off = bus.o
        if self.ram_off != self._after('Bus') + BUS_PRE_RAM:
            raise ParseError('internal: RAM offset mismatch')
        self.bios_off = self.ram_off + RAM_2MB_SIZE
        tail = Cursor(self.d, self.bios_off + BIOS_SIZE)
        self.memctrl = [tail.u32() for _ in range(9)]
        self.ram_size_reg = tail.u32()
        tty_len = tail.u32()
        tail.skip(tty_len)
        if tail.o != self.marker['DMA']:
            raise ParseError('Bus block ends at 0x%X, DMA marker at 0x%X'
                             % (tail.o, self.marker['DMA']))
        self.tty_line_buffer = self.d[tail.o - tty_len:tail.o].decode('latin-1')

        # GPU (fixed head + variable FIFO/blit buffer; must end on GPU-VRAM)
        self.gpu_off = self._after('GPU')
        self.gpu = self._parse_gpu(self.gpu_off)
        if self.gpu['_end'] != self.marker['GPU-VRAM']:
            raise ParseError('GPU block ends at 0x%X, GPU-VRAM marker at 0x%X'
                             % (self.gpu['_end'], self.marker['GPU-VRAM']))

        # VRAM directly follows the GPU-VRAM marker and must end on CDROM
        self.vram_off = self._after('GPU-VRAM')
        if self.vram_off + VRAM_SIZE != self.marker['CDROM']:
            raise ParseError('VRAM ends at 0x%X, CDROM marker at 0x%X'
                             % (self.vram_off + VRAM_SIZE, self.marker['CDROM']))

        # SPU RAM is the last thing SPU::DoState writes, so it ends on MDEC
        self.spu_ram_off = self.marker['MDEC'] - SPU_RAM_SIZE
        if self.spu_ram_off < self._after('SPU'):
            raise ParseError('SPU RAM would start before the SPU marker')
        self.spu_reg_bytes = self.spu_ram_off - self._after('SPU')

    # -- CPU ---------------------------------------------------------------
    def _parse_cpu(self, off):
        c = Cursor(self.d, off)
        r = {}
        r['pending_ticks'] = c.s32()
        r['downcount'] = c.s32()
        regs = [c.u32() for _ in range(36)]
        r['gpr'] = dict((GPR_NAMES[i], regs[i]) for i in range(32))
        r['gpr_raw'] = regs[:32]
        r['hi'], r['lo'], r['pc'], r['npc'] = regs[32], regs[33], regs[34], regs[35]
        cop0 = [c.u32() for _ in range(11)]
        r['cop0'] = dict(zip(COP0_NAMES, cop0))
        r['next_instruction'] = c.u32()
        r['current_instruction'] = c.u32()
        r['current_instruction_pc'] = c.u32()
        r['current_instruction_in_branch_delay_slot'] = c.b()
        r['current_instruction_was_branch_taken'] = c.b()
        r['next_instruction_is_branch_delay_slot'] = c.b()
        r['branch_was_taken'] = c.b()
        r['exception_raised'] = c.b()
        r['interrupt_delay'] = c.b()
        r['load_delay_reg'] = c.u8()
        r['load_delay_value'] = c.u32()
        r['next_load_delay_reg'] = c.u8()
        r['next_load_delay_value'] = c.u32()
        r['cache_control'] = c.u32()
        r['_dcache_off'] = c.o
        c.skip(DCACHE_SIZE)
        r['_gte_off'] = c.o
        gte = [c.u32() for _ in range(GTE_NUM_REGS)]
        r['gte_dr32'] = gte[:32]
        r['gte_cr32'] = gte[32:]
        r['_icache_tags_off'] = c.o
        c.skip(ICACHE_LINES * 4)
        r['_icache_data_off'] = c.o
        c.skip(ICACHE_SIZE)
        r['_end'] = c.o
        return r

    def dcache(self):
        """1 KB data cache, which on the PS1 is the scratchpad at 0x1F800000."""
        o = self.cpu['_dcache_off']
        return self.d[o:o + DCACHE_SIZE]

    # -- GPU ---------------------------------------------------------------
    def _parse_gpu(self, off):
        c = Cursor(self.d, off)
        g = {}
        g['GPUSTAT'] = c.u32()
        dm = {}
        dm['mode_reg'] = c.u16()
        dm['palette_reg'] = c.u16()
        dm['texture_window_value'] = c.u32()
        dm['texture_page_x'] = c.u32()
        dm['texture_page_y'] = c.u32()
        dm['texture_palette_x'] = c.u32()
        dm['texture_palette_y'] = c.u32()
        dm['texture_window_and_x'] = c.u8()
        dm['texture_window_and_y'] = c.u8()
        dm['texture_window_or_x'] = c.u8()
        dm['texture_window_or_y'] = c.u8()
        dm['texture_x_flip'] = c.b()
        dm['texture_y_flip'] = c.b()
        mr = dm['mode_reg']
        dm['decoded'] = {
            'texture_page_x_base': (mr & 0xF) * 64,
            'texture_page_y_base': ((mr >> 4) & 1) * 256,
            'transparency_mode': TRANSPARENCY_MODE[(mr >> 5) & 3],
            'texture_mode': TEXTURE_MODE[(mr >> 7) & 3],
            'dither_enable': bool((mr >> 9) & 1),
            'draw_to_displayed_field': bool((mr >> 10) & 1),
            'texture_disable': bool((mr >> 11) & 1),
        }
        g['draw_mode'] = dm
        g['drawing_area'] = {'left': c.u32(), 'top': c.u32(),
                             'right': c.u32(), 'bottom': c.u32()}
        # NOTE: this build's GPU::DoState writes m_drawing_offset.x, then .y,
        # then .x a SECOND time (gpu.cpp lines 158-160).  Three words go on the
        # wire; the duplicate is what makes the fixed head 160 and not 156 bytes.
        ox1 = c.s32()
        oy = c.s32()
        ox2 = c.s32()
        g['drawing_offset'] = {'x': ox1, 'y': oy}
        g['_drawing_offset_x_repeat'] = ox2
        g['console_is_pal'] = c.b()
        g['set_texture_disable_mask'] = c.b()
        crtc = {}
        crtc['display_address_start'] = c.u32()
        crtc['horizontal_display_range'] = c.u32()
        crtc['vertical_display_range'] = c.u32()
        crtc['dot_clock_divider'] = c.u16()
        crtc['display_width'] = c.u16()
        crtc['display_height'] = c.u16()
        crtc['display_origin_left'] = c.u16()
        crtc['display_origin_top'] = c.u16()
        crtc['display_vram_left'] = c.u16()
        crtc['display_vram_top'] = c.u16()
        crtc['display_vram_width'] = c.u16()
        crtc['display_vram_height'] = c.u16()
        crtc['horizontal_total'] = c.u16()
        crtc['horizontal_visible_start'] = c.u16()
        crtc['horizontal_visible_end'] = c.u16()
        crtc['horizontal_display_start'] = c.u16()
        crtc['horizontal_display_end'] = c.u16()
        crtc['vertical_total'] = c.u16()
        crtc['vertical_visible_start'] = c.u16()
        crtc['vertical_visible_end'] = c.u16()
        crtc['vertical_display_start'] = c.u16()
        crtc['vertical_display_end'] = c.u16()
        crtc['fractional_ticks'] = c.s32()
        crtc['current_tick_in_scanline'] = c.s32()
        crtc['current_scanline'] = c.u32()
        crtc['fractional_dot_ticks'] = c.s32()          # added in state version 46
        crtc['in_hblank'] = c.b()
        crtc['in_vblank'] = c.b()
        crtc['interlaced_field'] = c.u8()
        crtc['interlaced_display_field'] = c.u8()
        crtc['active_line_lsb'] = c.u8()
        crtc['display_address_start_x'] = crtc['display_address_start'] & 0x3FF
        crtc['display_address_start_y'] = (crtc['display_address_start'] >> 10) & 0x1FF
        g['crtc'] = crtc
        bs = c.u8()
        g['blitter_state'] = BLITTER_STATE.get(bs, bs)
        g['pending_command_ticks'] = c.s32()
        g['command_total_words'] = c.u32()
        g['GPUREAD_latch'] = c.u32()
        g['vram_transfer'] = {'x': c.u16(), 'y': c.u16(), 'width': c.u16(),
                              'height': c.u16(), 'col': c.u16(), 'row': c.u16()}
        n = c.u32()
        g['fifo'] = [struct.unpack_from('<Q', self.d, c.o + 8 * i)[0] for i in range(n)]
        c.skip(8 * n)
        m = c.u32()
        g['blit_buffer'] = [struct.unpack_from('<I', self.d, c.o + 4 * i)[0] for i in range(m)]
        c.skip(4 * m)
        g['blit_remaining_words'] = c.u32()
        g['render_command'] = c.u32()
        g['max_run_ahead'] = c.s32()
        g['fifo_size'] = c.u32()
        st = g['GPUSTAT']
        g['gpustat_decoded'] = {
            'texture_page_x_base': (st & 0xF) * 64,
            'texture_page_y_base': ((st >> 4) & 1) * 256,
            'texture_mode': TEXTURE_MODE[(st >> 7) & 3],
            'dither_enable': bool((st >> 9) & 1),
            'display_area_color_depth_24bit': bool((st >> 21) & 1),
            'vertical_resolution_480': bool((st >> 19) & 1),
            'horizontal_resolution_1': (st >> 17) & 3,
            'horizontal_resolution_2': (st >> 16) & 1,
            'vertical_interlace': bool((st >> 22) & 1),
            'display_disable': bool((st >> 23) & 1),
            'dma_direction': (st >> 29) & 3,
        }
        g['_end'] = c.o
        return g

    # -- component blobs ----------------------------------------------------
    def ram(self):
        return self.d[self.ram_off:self.ram_off + RAM_2MB_SIZE]

    def bios(self):
        return self.d[self.bios_off:self.bios_off + BIOS_SIZE]

    def vram(self):
        return self.d[self.vram_off:self.vram_off + VRAM_SIZE]

    def spu_ram(self):
        return self.d[self.spu_ram_off:self.spu_ram_off + SPU_RAM_SIZE]


# ---------------------------------------------------------------------------
# PNG (stdlib only)
# ---------------------------------------------------------------------------

def write_png(path, width, height, rgb_rows):
    raw = bytearray()
    for row in rgb_rows:
        raw.append(0)
        raw += row
    comp = zlib.compress(bytes(raw), 6)

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', comp))
        f.write(chunk(b'IEND', b''))


_EXP5 = [(i * 255 + 15) // 31 for i in range(32)]


def bgr555_rows(data, width, height, stride_px=None):
    """data is a run of halfwords; yield `height` rows of `width` RGB triples."""
    stride_px = stride_px or width
    rows = []
    for y in range(height):
        base = y * stride_px * 2
        row = bytearray(width * 3)
        for x in range(width):
            v = data[base + x * 2] | (data[base + x * 2 + 1] << 8)
            row[x * 3 + 0] = _EXP5[v & 0x1F]
            row[x * 3 + 1] = _EXP5[(v >> 5) & 0x1F]
            row[x * 3 + 2] = _EXP5[(v >> 10) & 0x1F]
        rows.append(bytes(row))
    return rows


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_info(path):
    s = SaveState(path)
    print('file                  %s (%d bytes)' % (os.path.basename(path), len(s.d)))
    print('magic/version         0x%08X / %d' % (s.magic, s.version))
    print('title / game code     %r / %r' % (s.title, s.game_code))
    print('media                 %s (subimage %d)' % (s.media_filename, s.media_subimage_index))
    print('screenshot            %dx%d, %d bytes @ 0x%X'
          % (s.screenshot_width, s.screenshot_height, s.screenshot_size, s.offset_to_screenshot))
    print('data blob             0x%X .. 0x%X (%d bytes), compression %d'
          % (s.data_begin, s.data_end, s.data_uncompressed_size, s.data_compression_type))
    print('padding after data    %d bytes (libretro fixed 11 MiB buffer)' % (len(s.d) - s.data_end))
    print('region / frame        %s / %d (internal %d)'
          % (CONSOLE_REGION.get(s.region, s.region), s.frame_number, s.internal_frame_number))
    print()
    print('marker map (absolute file offsets):')
    for name in MARKERS:
        print('  %-20s 0x%08X' % (name, s.marker[name]))
    print()
    print('components:')
    print('  %-10s 0x%08X  %8d' % ('CPU', s.cpu_off, s.cpu['_end'] - s.cpu_off))
    print('  %-10s 0x%08X  %8d' % ('RAM', s.ram_off, RAM_2MB_SIZE))
    print('  %-10s 0x%08X  %8d' % ('BIOS', s.bios_off, BIOS_SIZE))
    print('  %-10s 0x%08X  %8d' % ('GPU regs', s.gpu_off, s.gpu['_end'] - s.gpu_off))
    print('  %-10s 0x%08X  %8d' % ('VRAM', s.vram_off, VRAM_SIZE))
    print('  %-10s 0x%08X  %8d' % ('SPU regs', s._after('SPU'), s.spu_reg_bytes))
    print('  %-10s 0x%08X  %8d' % ('SPU RAM', s.spu_ram_off, SPU_RAM_SIZE))
    print()
    c = s.cpu
    print('CPU  pc=%08X npc=%08X hi=%08X lo=%08X' % (c['pc'], c['npc'], c['hi'], c['lo']))
    print('     cur_instr_pc=%08X cur_instr=%08X' % (c['current_instruction_pc'], c['current_instruction']))
    print('     sp=%08X gp=%08X ra=%08X fp=%08X'
          % (c['gpr']['sp'], c['gpr']['gp'], c['gpr']['ra'], c['gpr']['fp']))
    print('     SR=%08X CAUSE=%08X EPC=%08X PRID=%08X'
          % (c['cop0']['SR'], c['cop0']['CAUSE'], c['cop0']['EPC'], c['cop0']['PRID']))
    print('     load_delay_reg=%d next_load_delay_reg=%d exception_raised=%s'
          % (c['load_delay_reg'], c['next_load_delay_reg'], c['exception_raised']))
    print()
    g = s.gpu
    cr = g['crtc']
    print('GPU  GPUSTAT=%08X  blitter=%s  fifo=%d words  blit_buffer=%d words'
          % (g['GPUSTAT'], g['blitter_state'], len(g['fifo']), len(g['blit_buffer'])))
    print('     draw area  (%d,%d)-(%d,%d)  offset (%d,%d)'
          % (g['drawing_area']['left'], g['drawing_area']['top'],
             g['drawing_area']['right'], g['drawing_area']['bottom'],
             g['drawing_offset']['x'], g['drawing_offset']['y']))
    print('     tpage base (%d,%d) mode %s  clut (%d,%d)  texwin and(%d,%d) or(%d,%d)'
          % (g['draw_mode']['texture_page_x'], g['draw_mode']['texture_page_y'],
             g['draw_mode']['decoded']['texture_mode'],
             g['draw_mode']['texture_palette_x'], g['draw_mode']['texture_palette_y'],
             g['draw_mode']['texture_window_and_x'], g['draw_mode']['texture_window_and_y'],
             g['draw_mode']['texture_window_or_x'], g['draw_mode']['texture_window_or_y']))
    print('     display start VRAM (%d,%d)  vram rect (%d,%d) %dx%d  screen %dx%d  24bit=%s'
          % (cr['display_address_start_x'], cr['display_address_start_y'],
             cr['display_vram_left'], cr['display_vram_top'],
             cr['display_vram_width'], cr['display_vram_height'],
             cr['display_width'], cr['display_height'],
             g['gpustat_decoded']['display_area_color_depth_24bit']))
    return 0


def cmd_extract(path, outdir):
    s = SaveState(path)
    os.makedirs(outdir, exist_ok=True)

    def put(name, blob):
        p = os.path.join(outdir, name)
        with open(p, 'wb') as f:
            f.write(blob)
        print('  %-12s %8d bytes  %s' % (name, len(blob), p))

    print('extract %s -> %s' % (path, outdir))
    put('ram.bin', s.ram())
    put('vram.bin', s.vram())
    put('spuram.bin', s.spu_ram())
    put('bios.bin', s.bios())
    put('scratchpad.bin', s.dcache())

    cpu = dict(s.cpu)
    for k in list(cpu):
        if k.startswith('_'):
            del cpu[k]
    cpu['gpr_hex'] = ['%08X' % v for v in cpu['gpr_raw']]
    cpu['gte_dr32_hex'] = ['%08X' % v for v in cpu['gte_dr32']]
    cpu['gte_cr32_hex'] = ['%08X' % v for v in cpu['gte_cr32']]
    cpu['_source'] = {'savestate': os.path.abspath(path),
                      'state_version': s.version,
                      'frame_number': s.frame_number,
                      'ram_base': '0x80000000'}
    with open(os.path.join(outdir, 'cpu.json'), 'w') as f:
        json.dump(cpu, f, indent=2, sort_keys=True)
    print('  cpu.json      pc=%08X sp=%08X' % (cpu['pc'], cpu['gpr']['sp']))

    gpu = dict(s.gpu)
    del gpu['_end']
    gpu['_source'] = {'savestate': os.path.abspath(path), 'state_version': s.version}
    with open(os.path.join(outdir, 'gpu.json'), 'w') as f:
        json.dump(gpu, f, indent=2, sort_keys=True)
    print('  gpu.json      GPUSTAT=%08X' % gpu['GPUSTAT'])
    return 0


def cmd_vram_png(path, out):
    s = SaveState(path)
    v = s.vram()
    write_png(out, VRAM_WIDTH, VRAM_HEIGHT, bgr555_rows(v, VRAM_WIDTH, VRAM_HEIGHT))
    print('wrote %s (%dx%d)' % (out, VRAM_WIDTH, VRAM_HEIGHT))

    cr = s.gpu['crtc']
    w = max(1, min(cr['display_vram_width'], VRAM_WIDTH))
    h = max(1, min(cr['display_vram_height'], VRAM_HEIGHT))
    x0, y0 = cr['display_vram_left'], cr['display_vram_top']
    rows = []
    for y in range(h):
        base = ((y0 + y) % VRAM_HEIGHT) * VRAM_WIDTH * 2
        row = bytearray(w * 3)
        for x in range(w):
            o = base + ((x0 + x) % VRAM_WIDTH) * 2
            p = v[o] | (v[o + 1] << 8)
            row[x * 3 + 0] = _EXP5[p & 0x1F]
            row[x * 3 + 1] = _EXP5[(p >> 5) & 0x1F]
            row[x * 3 + 2] = _EXP5[(p >> 10) & 0x1F]
        rows.append(bytes(row))
    disp = out[:-4] + '.display.png' if out.lower().endswith('.png') else out + '.display.png'
    write_png(disp, w, h, rows)
    print('wrote %s (display area %dx%d at VRAM %d,%d)' % (disp, w, h, x0, y0))
    return 0


GTP_DEFAULT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                           'work', 'disc_us', 'DATA', 'G_OBJ01.GTP')


def _slot(v, x, y):
    o = (y * VRAM_WIDTH + x) * 2
    return v[o:o + 32]


def cmd_cluts(path, outdir, gtp_path=None):
    """Locate the 4bpp CLUTs that are resident in VRAM and identify them.

    PS1 CLUT id -> VRAM: x = (id & 0x3F) * 16, y = id >> 6.  A 4bpp CLUT is 16
    consecutive halfwords, so only x that are multiples of 16 can host one; that
    is 64 x 512 = 32768 candidate slots in a VRAM image.

    Two passes:
      1. residency check for DATA/G_OBJ01.GTP as a whole, at the placement the
         game actually used (derived from pass 2 and then asserted here);
      2. exhaustive sweep matching every candidate slot against every GTP entry.
    """
    s = SaveState(path)
    os.makedirs(outdir, exist_ok=True)
    v = s.vram()

    gtp_path = gtp_path or GTP_DEFAULT
    gtp = None
    if os.path.isfile(gtp_path):
        with open(gtp_path, 'rb') as f:
            gtp = f.read()
        if len(gtp) != 16384:
            print('warning: %s is %d bytes, expected 16384' % (gtp_path, len(gtp)))
    else:
        print('warning: GTP not found at %s, VRAM sweep only' % gtp_path)

    print('VRAM CLUT report for %s (frame %d)' % (os.path.basename(path), s.frame_number))

    # -- pass 1: is the whole GTP bank resident as a 64x128 VRAM rectangle? ----
    bank = None
    if gtp:
        best = None
        # the bank is 4 CLUTs wide x 128 rows; try every legal top-left
        for bx in range(0, VRAM_WIDTH - 63, 16):
            for by in range(0, VRAM_HEIGHT - 127):
                if _slot(v, bx, by) != gtp[0:32]:
                    continue
                n_ok = sum(1 for n in range(512)
                           if _slot(v, bx + (n % 4) * 16, by + n // 4) == gtp[n * 32:n * 32 + 32])
                if best is None or n_ok > best[2]:
                    best = (bx, by, n_ok)
                if n_ok == 512:
                    break
            if best and best[2] == 512:
                break
        if best:
            bx, by, n_ok = best
            bank = best
            zeros = sum(1 for n in range(512) if gtp[n * 32:n * 32 + 32] == b'\0' * 32)
            print('  G_OBJ01.GTP bank found at VRAM (%d,%d), 64x128 px' % (bx, by))
            print('    %d/512 entries byte-identical (%d of them are all-zero in the file)'
                  % (n_ok, zeros))
            print('    CLUT id of GTP entry n = ((%d + n//4) << 6) | (%d + n%%4)'
                  % (by, bx // 16))
            print('    i.e. entry 0 -> id %d (0x%04X), entry 511 -> id %d (0x%04X)'
                  % (((by) << 6) | (bx // 16), ((by) << 6) | (bx // 16),
                     ((by + 127) << 6) | (bx // 16 + 3), ((by + 127) << 6) | (bx // 16 + 3)))
        else:
            print('  G_OBJ01.GTP bank NOT found as a contiguous 64x128 rectangle')

    # -- pass 2: exhaustive slot sweep ---------------------------------------
    gtp_index = {}
    if gtp:
        for i in range(len(gtp) // 32):
            gtp_index.setdefault(gtp[i * 32:i * 32 + 32], []).append(i)

    nonzero = 0
    matches = []          # (clut_id, x, y, [gtp slots])
    occupied = []         # (clut_id, x, y) for any non-zero slot
    for y in range(VRAM_HEIGHT):
        rowbase = y * VRAM_WIDTH * 2
        for xb in range(64):
            o = rowbase + xb * 32
            blob = v[o:o + 32]
            if blob == b'\0' * 32:
                continue
            nonzero += 1
            clut_id = (y << 6) | xb
            occupied.append((clut_id, xb * 16, y))
            hit = gtp_index.get(blob)
            if hit:
                matches.append((clut_id, xb * 16, y, hit))

    print('  candidate 16-entry slots: %d, non-zero: %d' % (64 * VRAM_HEIGHT, nonzero))
    if gtp:
        print('  distinct GTP payloads: %d of %d entries' % (len(gtp_index), len(gtp) // 32))
        print('  VRAM slots byte-identical to some GTP entry: %d' % len(matches))
        outside = [m for m in matches
                   if not (bank and bank[0] <= m[1] < bank[0] + 64
                           and bank[1] <= m[2] < bank[1] + 128)]
        print('  ... of which outside the GTP bank rectangle: %d' % len(outside))

    rep = os.path.join(outdir, 'cluts.txt')
    with open(rep, 'w') as f:
        f.write('# VRAM 4bpp CLUT report for %s\n' % os.path.abspath(path))
        f.write('# state version %d, frame %d\n' % (s.version, s.frame_number))
        f.write('# clut_id = (y<<6)|(x>>4)\n')
        if bank:
            f.write('# G_OBJ01.GTP bank resident at VRAM (%d,%d) 64x128, %d/512 exact\n'
                    % (bank[0], bank[1], bank[2]))
            f.write('#\n# gtp_entry vram_x vram_y clut_id  16 BGR555 entries\n')
            for n in range(512):
                x = bank[0] + (n % 4) * 16
                y = bank[1] + n // 4
                blob = _slot(v, x, y)
                same = 'OK ' if blob == gtp[n * 32:n * 32 + 32] else 'DIFF'
                ent = ' '.join('%04X' % struct.unpack_from('<H', blob, i * 2)[0] for i in range(16))
                f.write('%s %3d  %4d %4d  %5d 0x%04X  %s\n'
                        % (same, n, x, y, (y << 6) | (x >> 4), (y << 6) | (x >> 4), ent))
        f.write('\n# every VRAM slot that equals some GTP entry (sweep)\n')
        f.write('# clut_id vram_x vram_y gtp_entries\n')
        for clut_id, x, y, hit in matches:
            f.write('%5d 0x%04X  %4d %4d  gtp=%s\n'
                    % (clut_id, clut_id, x, y, ','.join(str(i) for i in hit)))
        f.write('\n# all non-zero 16-entry slots in VRAM (includes frame-buffer pixels)\n')
        for clut_id, x, y in occupied:
            f.write('%5d 0x%04X  %4d %4d\n' % (clut_id, clut_id, x, y))
    print('  wrote %s' % rep)

    if bank:
        # the bank as an image: 512 CLUTs, one per row, 16 swatches wide
        rows = []
        for n in range(512):
            x = bank[0] + (n % 4) * 16
            y = bank[1] + n // 4
            rows.append(bgr555_rows(_slot(v, x, y), 16, 1)[0])
        png = os.path.join(outdir, 'gtp_bank_from_vram.png')
        write_png(png, 16, 512, rows)
        print('  wrote %s (row n = GTP entry n as uploaded)' % png)

        png2 = os.path.join(outdir, 'gtp_bank_region.png')
        sub = []
        for y in range(bank[1], bank[1] + 128):
            o = (y * VRAM_WIDTH + bank[0]) * 2
            sub.append(bgr555_rows(v[o:o + 128], 64, 1)[0])
        write_png(png2, 64, 128, sub)
        print('  wrote %s (the raw 64x128 VRAM rectangle)' % png2)
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    try:
        if cmd == 'info':
            return cmd_info(sys.argv[2])
        if cmd == 'extract':
            return cmd_extract(sys.argv[2], sys.argv[3])
        if cmd == 'vram-png':
            return cmd_vram_png(sys.argv[2], sys.argv[3])
        if cmd == 'cluts':
            return cmd_cluts(sys.argv[2], sys.argv[3],
                             sys.argv[4] if len(sys.argv) > 4 else None)
    except ParseError as e:
        print('PARSE ERROR: %s' % e)
        return 2
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())
