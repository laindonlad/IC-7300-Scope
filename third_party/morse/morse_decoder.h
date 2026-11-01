#pragma once
// Morse (CW) decoder: audio in, text out. Plain C++, no Arduino, so it also builds on a PC
// (tools/morse_test.cpp) to try it on synthesised audio.
//
// 1. A bank of Goertzel filters (TONE_LO..TONE_HI, Hann-windowed) measures the last BLOCK
//    samples every HOP. Each bin scores its average lift over the bank's median less the lift
//    it keeps in the gaps, so a keyed tone stands out and a steady whistle doesn't. The tone is
//    the best bin; it changes only when another beats it clearly. A parabola through it and
//    its neighbours places the tone between bin centres. The squelch opens while the tone's
//    score stands CONF_DB over the median score, which noise alone doesn't reach.
// 2. A narrow band-pass around that tone: the audio is mixed down to 0Hz with the tone's
//    frequency (I and Q), averaged in groups of DECIM samples, and low-passed by a 4th-order
//    Butterworth filter whose cut-off follows the speed (FC_PER_DIT / dit: ~13Hz either side
//    at 20 WPM, where a bin is ~70Hz wide). While the key is down, the phase turning from hop
//    to hop says how far off the tone the mixer is, and nudges it there (AFC), so a drifting
//    tone stays in the pass band.
//    Chirp: a sender whose tone starts off and bends onto it as the key goes down (700Hz
//    settling to 650 in some tens of ms) would key the band-pass late, or not at all for a
//    dit. Wider chirp channels (±32Hz at least) lie on the tone and every ~48Hz either side,
//    out to CHIRP_HZ. Those that light up just before marks start, on one side of the tone
//    only, are learnt from the marks heard; from then on the key also goes down on them, and
//    the AFC waits for the chirp to settle rather than following it.
// 3. The band-pass's power (in dB) against a noise floor (its average while the key is up) and
//    a peak (up fast, down slowly) gives key down/up, with hysteresis; the threshold is halfway
//    in amplitude. A click (the bank's median jumps, and the tone's bin with it) leaves the key
//    as it was. A change counts only once it has lasted a quarter of a dit (DEBOUNCE hops at
//    least), and a mark only if the squelch opened while it lasted.
// 4. Mark lengths fall in two groups, dits and dahs. Each mark updates the nearer of the two
//    estimates, and the pair is kept between 2:1 and 4.5:1, so a change of speed pulls both
//    along; a split of the latest marks into two groups catches a sudden one. Noise and the
//    filter make marks short and gaps long by the same amount, which is measured and taken
//    off. Gaps under 2 dits are within a character; longer ones are sorted into letter and
//    word gaps the same way, so Farnsworth spacing (fast letters, long gaps) reads right too.
// 5. When the speed or the letter gap turns out a third off what it seemed, the latest marks
//    that fit the new one are decoded again, so the start of a transmission, or what came just
//    after a change of speed, reads right too.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

class MorseDecoder {
 public:
  static constexpr uint32_t RATE = 16000;
  static constexpr int BLOCK = 256;  // 16ms window (Hann): each bin is ~70Hz wide
  static constexpr int HOP = 64;  // measured every 4ms over the last 16ms
  static constexpr float HOP_MS = 1000.0f * HOP / RATE;
  static constexpr float TONE_LO = 400, TONE_HI = 1200, TONE_STEP = 50;
  static constexpr int BINS = (int)((TONE_HI - TONE_LO) / TONE_STEP) + 1;
  static constexpr int DECIM = 16;  // the band-pass runs at 1kHz
  static constexpr int TEXT = 200;

  // Every character and prosign, as text and as dots and dashes. Prosigns are written <SK>.
  // Where a prosign and a character share a code, the first in the list is what's decoded:
  // <AR>, <KN>, <AS> and <BT> rather than +, (, & and =. The later ones (and the aliases <CT> and
  // <VE>) can still be sent.
  struct Sign {
    const char* text;
    const char* code;
  };
  static constexpr Sign SIGNS[] = {
    {"<AR>", ".-.-."}, {"<BT>", "-...-"}, {"<KN>", "-.--."}, {"<AS>", ".-..."}, {"<SK>", "...-.-"}, {"<BK>", "-...-.-"},
    {"<CL>", "-.-..-.."}, {"<KA>", "-.-.-"}, {"<SN>", "...-."}, {"<AA>", ".-.-"}, {"<SOS>", "...---..."},
    {"<HH>", "........"},
    {"A", ".-"}, {"B", "-..."}, {"C", "-.-."}, {"D", "-.."}, {"E", "."}, {"F", "..-."}, {"G", "--."},
    {"H", "...."}, {"I", ".."}, {"J", ".---"}, {"K", "-.-"}, {"L", ".-.."}, {"M", "--"}, {"N", "-."},
    {"O", "---"}, {"P", ".--."}, {"Q", "--.-"}, {"R", ".-."}, {"S", "..."}, {"T", "-"}, {"U", "..-"},
    {"V", "...-"}, {"W", ".--"}, {"X", "-..-"}, {"Y", "-.--"}, {"Z", "--.."},
    {"0", "-----"}, {"1", ".----"}, {"2", "..---"}, {"3", "...--"}, {"4", "....-"}, {"5", "....."},
    {"6", "-...."}, {"7", "--..."}, {"8", "---.."}, {"9", "----."},
    {".", ".-.-.-"}, {",", "--..--"}, {"?", "..--.."}, {"/", "-..-."}, {"=", "-...-"}, {"-", "-....-"},
    {"'", ".----."}, {")", "-.--.-"}, {":", "---..."}, {";", "-.-.-."}, {"\"", ".-..-."}, {"_", "..--.-"},
    {"@", ".--.-."}, {"!", "-.-.--"}, {"$", "...-..-"},
    {"+", ".-.-."}, {"(", "-.--."}, {"&", ".-..."}, {"<CT>", "-.-.-"}, {"<VE>", "...-."},
  };
  static constexpr int NSIGNS = sizeof(SIGNS) / sizeof(SIGNS[0]);
  static constexpr int MAX_ELEMENTS = 9;  // SOS and <CL>

  // The alphabet received Morse is read in (UTF-8). Its signs are looked up before SIGNS, so
  // where they share a code, they win. They're only decoded: the tutor teaches and sends SIGNS
  // alone, and Progress is sized by it.
  //  ACCENTED: the letters German, Scandinavian, Spanish, French and Polish operators add. One
  //    letter per code where several share it (Ä is also Æ and Ą, Ö is also Ø and Ó, É is also
  //    Ę, È is also Ł, Ñ is also Ń, Ç is also Ć). Ä takes <AA>'s code.
  //  CYRILLIC: Russian. The Latin letters' codes are the matching Cyrillic letters, and the
  //    punctuation is Russia's own: "......" a full stop (seven dits or more are still the error
  //    sign), ".-.-.-" a comma, "--..--" an exclamation mark.
  enum Alphabet : uint8_t { INTERNATIONAL, ACCENTED, CYRILLIC, ALPHABETS };
  static constexpr Sign ACCENTED_SIGNS[] = {
    {"Ä", ".-.-"}, {"Å", ".--.-"}, {"Ç", "-.-.."}, {"CH", "----"}, {"É", "..-.."}, {"È", ".-..-"},
    {"Ñ", "--.--"}, {"Ö", "---."}, {"Ü", "..--"}, {"Ś", "...-..."}, {"Ź", "--..-."}, {"Ż", "--..-"},
  };
  static constexpr Sign CYRILLIC_SIGNS[] = {
    {"А", ".-"}, {"Б", "-..."}, {"В", ".--"}, {"Г", "--."}, {"Д", "-.."}, {"Е", "."}, {"Ж", "...-"},
    {"З", "--.."}, {"И", ".."}, {"Й", ".---"}, {"К", "-.-"}, {"Л", ".-.."}, {"М", "--"}, {"Н", "-."},
    {"О", "---"}, {"П", ".--."}, {"Р", ".-."}, {"С", "..."}, {"Т", "-"}, {"У", "..-"}, {"Ф", "..-."},
    {"Х", "...."}, {"Ц", "-.-."}, {"Ч", "---."}, {"Ш", "----"}, {"Щ", "--.-"}, {"Ъ", "--.--"},
    {"Ы", "-.--"}, {"Ь", "-..-"}, {"Э", "..-.."}, {"Ю", "..--"}, {"Я", ".-.-"},
    {".", "......"}, {",", ".-.-.-"}, {"!", "--..--"},
  };
  // For every decoder; it takes effect from the next character.
  static void setAlphabet(uint8_t a) { alphabet_ = a < ALPHABETS ? a : INTERNATIONAL; }
  static uint8_t alphabet() { return alphabet_; }

  // Only tones from `lo` to `hi` Hz can be the tone (the whole bank by default). The digital
  // modes' CW gives each signal a channel of its own, centred on its tone: a neighbour that
  // leaks into the channel's edge mustn't take over when its own signal stops.
  void setToneRange(float lo, float hi) {
    binLo_ = (int)ceilf((lo - TONE_LO) / TONE_STEP);
    binHi_ = (int)((hi - TONE_LO) / TONE_STEP);
    if (binLo_ < 0) binLo_ = 0;
    if (binHi_ > BINS - 1) binHi_ = BINS - 1;
    if (binHi_ < binLo_) binHi_ = binLo_;
  }

  // A sign's code: a leading 1 bit, then a bit per element (0 dit, 1 dah).
  static uint32_t encode(const char* dots) {
    uint32_t k = 1;
    for (; *dots; dots++) k = k << 1 | (*dots == '-');
    return k;
  }

  // The sign `p` starts with (letters in either case): its place in SIGNS, and its length in
  // `len`; -1 (and `len` 1) if none.
  static int signAt(const char* p, int& len) {
    for (int i = 0; i < NSIGNS; i++) {
      const char* t = SIGNS[i].text;
      int n = 0;
      while (t[n] && p[n] && (p[n] >= 'a' && p[n] <= 'z' ? p[n] - 32 : p[n]) == t[n]) n++;
      if (!t[n]) {
        len = n;
        return i;
      }
    }
    len = 1;
    return -1;
  }

  MorseDecoder() {
    for (int i = 0; i < BINS; i++) {
      float hz = TONE_LO + i * TONE_STEP;
      coef_[i] = 2 * cosf(2 * 3.14159265f * hz / RATE);
    }
    for (int i = 0; i < BLOCK; i++) win_[i] = 0.5f - 0.5f * cosf(2 * 3.14159265f * (i + 0.5f) / BLOCK);
    buildTable();
    reset();
  }

  void reset() {
    for (int i = 0; i < BINS; i++) score_[i] = low_[i] = shape_[i] = 0;
    hops_ = 0;
    bin_ = -1;
    afc_ = false;
    oscC_ = 1;
    oscS_ = 0;
    tune(700);
    fc_ = 0;
    nOnsets_ = 0;
    sinceChirp_ = 255;
    cutoff(FC_MAX);
    for (Biquad& f : lp_) f.clear();
    for (auto& ch : chLp_)
      for (Biquad& f : ch) f.clear();
    rotC_ = 1;
    rotS_ = 0;
    accI_ = accQ_ = env_ = 0;
    for (float& e : chEnv_) e = 0;
    nAcc_ = nEnv_ = 0;
    zI_ = zQ_ = pI_ = pQ_ = 0;
    medAvg_ = -200;
    conf_ = 0;
    open_ = false;
    heard_ = false;
    gapHops_ = 0;
    clickHops_ = 0;
    floor_ = 0;
    peak_ = 0;
    primed_ = false;
    down_ = false;
    raw_ = false;
    rawRun_ = 0;
    runHops_ = 0;
    dit_ = 60;
    dah_ = 180;
    charGap_ = 3;
    nHist_ = 0;
    nGap_ = 0;
    rawDit_ = rawGap_ = 0;
    nRawDit_ = nRawGap_ = 0;
    skew_ = 0;
    wordGap_ = 7;
    code_ = 1;
    spaced_ = true;
    text_[0] = 0;
    len_ = 0;
    seq_ = 0;
    scrolled_ = 0;
    nLog_ = 0;
    ditRef_ = dit_;
    charRef_ = charGap_;
  }

  // Feeds mono samples; any count.
  void feed(const int16_t* s, int n) {
    while (n > 0) {
      int take = BLOCK - fill_ < n ? BLOCK - fill_ : n;
      for (int i = 0; i < take; i++) {
        float x = s[i] / 32768.0f;
        blk_[fill_ + i] = x;
        mix(x);
      }
      fill_ += take;
      s += take;
      n -= take;
      if (fill_ == BLOCK) {
        block();
        memmove(blk_, blk_ + HOP, (BLOCK - HOP) * sizeof(float));
        fill_ = BLOCK - HOP;
      }
    }
  }

  // Decoded text, oldest first (at most TEXT - 1 characters); seq() changes with it.
  const char* text() const { return text_; }
  uint32_t seq() const { return seq_; }
  // Characters dropped off the front of text() since the last reset.
  uint32_t scrolled() const { return scrolled_; }
  float toneHz() const { return bin_ < 0 ? 0 : hz_; }
  float wpm() const { return 1200.0f / dit_; }
  float snrDb() const { return primed_ ? peak_ - floor_ : 0; }
  float levelDb() const { return level_; }
  bool keyDown() const { return down_; }
  float confDb() const { return conf_; }
  bool locked() const { return bin_ >= 0 && open_ && snrDb() >= MIN_SNR_DB; }

 private:
  static constexpr float MIN_SNR_DB = 14;  // below this, nothing is keyed
  static constexpr float HYST_DB = 1;
  static constexpr float MIN_THR_DB = 5;
  static constexpr float SCORE_K = 0.01f;  // ~0.4s to settle on a tone
  static constexpr float SCORE_FAST = 0.02f;  // while there's none: quicker to find one
  static constexpr uint32_t DEBOUNCE = 2;  // hops at least: 8ms, so up to ~60 WPM
  static constexpr float SWITCH_RATIO = 1.5f;
  static constexpr float LOW_DOWN = 0.05f, LOW_UP = 0.002f;  // ~80ms down, ~2s up
  static constexpr uint32_t WARM_HOPS = 125;  // 0.5s
  static constexpr float SHAPE_K = 0.001f, SHAPE_FAST = 0.05f;
  static constexpr int SKEW_AFTER = 8;  // dits and gaps heard before skew_ is trusted
  static constexpr float CONF_DB = 2.0f;  // the squelch opens at this tone confidence
  static constexpr float FC_PER_DIT = 0.8f;  // the band-pass's cut-off (either side), per dit/s
  static constexpr float FC_MIN = 12, FC_MAX = 80;  // Hz
  static constexpr float AFC_K = 0.1f;
  static constexpr float CHIRP_HZ = 150;  // how far off the tone a mark may start (chirp)
  static constexpr float CHIRP_STEP = 1.5f;  // channel spacing, per Hz of cut-off
  static constexpr float CHIRP_FC = 32;  // Hz: the chirp channels' least cut-off
  static constexpr uint8_t CH_WARM = 25;  // hops: 0.1s
  static constexpr float CHIRP_FALL = 0.3f;  // dB a hop: chRecent_ holds a peak for ~30 hops
  static constexpr float CHIRP_K = 0.1f;  // learnt over the latest ~10 marks
  static constexpr int CHIRP_SKIP = 2;  // marks heard before they're learnt from
  static constexpr int CHIRP_AFTER = 6;  // marks heard before any channel is taken in
  // A channel is taken in when it lights up before marks this much more than the one opposite
  // (and than the one on the tone, less CHIRP_BELOW_DB), and let go below CHIRP_OUT_DB.
  static constexpr float CHIRP_IN_DB = 5, CHIRP_OUT_DB = 2;
  static constexpr float CHIRP_BELOW_DB = 14;
  static constexpr uint8_t CHIRP_SETTLE = 4;  // hops after a chirp before the AFC follows
  static constexpr float CLICK_DB = 12;  // the bank's median this far over its usual: a click
  static constexpr uint32_t CLICK_HOLD = 3;  // hops the key stays put after one

  // One 2nd-order section (transposed direct form II), for I and Q at once.
  struct Biquad {
    float b0, b1, b2, a1, a2;
    float i1, i2, q1, q2;
    void clear() { i1 = i2 = q1 = q2 = 0; }
    void set(float fc, float fs, float q) {  // a low-pass (RBJ's cookbook)
      float w = 2 * 3.14159265f * fc / fs, cw = cosf(w), al = sinf(w) / (2 * q), a0 = 1 + al;
      b0 = b2 = (1 - cw) / 2 / a0;
      b1 = (1 - cw) / a0;
      a1 = -2 * cw / a0;
      a2 = (1 - al) / a0;
    }
    void run(float& i, float& q) {
      float yi = b0 * i + i1, yq = b0 * q + q1;
      i1 = b1 * i - a1 * yi + i2;
      i2 = b2 * i - a2 * yi;
      q1 = b1 * q - a1 * yq + q2;
      q2 = b2 * q - a2 * yq;
      i = yi;
      q = yq;
    }
  };

  float coef_[BINS];
  float score_[BINS];
  float low_[BINS];     // each bin's lift in the gaps: down fast, up slowly
  float shape_[BINS];   // each bin's usual level over the median without a tone, dB
  uint32_t hops_;       // since reset(), up to WARM_HOPS
  float blk_[BLOCK];
  float win_[BLOCK];
  float x_[BLOCK];      // the block, windowed (here, not on the sound task's small stack)
  int fill_ = 0;
  int bin_;
  float hz_;            // the tone, between bin centres: what the mixer is tuned to
  bool afc_;            // hz_ has been fine-tuned on marks since the bin was chosen
  float oscC_, oscS_, stepC_, stepS_;  // the mixer's oscillator and its turn per sample
  float fc_;
  Biquad lp_[2];        // Q 0.541 and 1.307: a 4th-order Butterworth
  float accI_, accQ_, env_;
  // Chirp channels: wider band-passes on the tone and either side of it, so a mark that starts
  // off the tone and sweeps onto it (chirp) is heard from its start. Channel c is offset
  // (c - CH) x chStep_ Hz; nCh_ each side are in use.
  static constexpr int CH = 4, NCH = 2 * CH + 1;
  Biquad chLp_[NCH][2];
  float chStep_, chFc_;  // their spacing and cut-off
  int nCh_;
  float rotC_, rotS_, rotStepC_, rotStepS_;  // turns by chStep_ per decimated sample
  float chEnv_[NCH];
  float chFloor_[NCH];  // each channel's level with only noise in it, dB
  uint8_t chWarm_[NCH];  // hops until a channel that came into use counts
  float chRecent_[NCH];  // each channel's peak over its floor, lately, dB
  float chPre_[NCH];     // chRecent_ when the key last started down
  float chOnset_[NCH];   // chPre_ for the marks heard, usually
  float chGap_[NCH];     // over its floor in the gaps, usually: ~0 but for another signal
  bool chIn_[NCH];       // a chirp lights it up: it counts
  int nOnsets_;
  uint8_t sinceChirp_;   // hops since a channel off the tone was the loudest
  int nAcc_, nEnv_;
  float zI_, zQ_, pI_, pQ_;  // the band-pass's output now, and at the last hop
  float medAvg_;        // the bank's median, usually
  uint32_t clickHops_;
  float conf_;          // the tone's score over the median score, dB: ~0 for noise
  bool open_;           // the squelch: a tone is there
  bool heard_;          // the squelch was open at some time in this mark
  uint32_t gapHops_;    // the gap before this mark, which goes on if the mark wasn't one
  float floor_, peak_, level_ = -200;
  bool primed_;
  bool down_;           // debounced
  bool raw_;            // this hop's
  uint32_t rawRun_;     // hops raw_ has held
  uint32_t runHops_;    // hops in the current (debounced) mark or gap
  float dit_, dah_;     // ms
  float charGap_, wordGap_;  // in dits
  float rawDit_, rawGap_;  // ms as heard (before skew_): dits, and gaps within a letter
  float skew_;          // ms that marks are heard short and gaps long
  int nRawDit_, nRawGap_;
  static constexpr int HIST = 16;
  float hist_[HIST];    // the latest marks, ms
  int nHist_;
  float gHist_[HIST];   // the latest letter and word gaps, dits
  int nGap_;
  uint32_t code_;       // 1, then a bit per element: 0 dit, 1 dah; 0 if it's too long to be anything
  bool spaced_;         // a word space was already written
  char text_[TEXT];
  int len_;
  uint32_t seq_;
  uint32_t scrolled_;   // characters dropped off the front of text_
  uint16_t codes_[NSIGNS];  // SIGNS' codes
  // The latest marks, with the gap before each, for decoding them again when the speed turns
  // out to be other than it seemed (at the start, or after a change).
  static constexpr int LOG = 32;
  static constexpr uint32_t NOT_FIRST = 0xffffffff;
  struct Element {
    float gap, mark;  // ms as heard
    uint32_t at;      // where its character starts in the text (scrolled_ + index), if it's the first
  };
  Element log_[LOG];
  int nLog_;
  float ditRef_, charRef_;  // dit_ and charGap_ when the log was last decoded

  void buildTable() {
    for (int i = 0; i < NSIGNS; i++) codes_[i] = encode(SIGNS[i].code);
  }

  static inline volatile uint8_t alphabet_ = INTERNATIONAL;
  int binLo_ = 0, binHi_ = BINS - 1;  // setToneRange

  void put(char c) {
    if (len_ == TEXT - 1) {  // a whole character (all its UTF-8 bytes) goes off the front
      int n = 1;
      while (n < len_ && (text_[n] & 0xC0) == 0x80) n++;
      memmove(text_, text_ + n, len_ - n + 1);
      len_ -= n;
      scrolled_ += n;
    }
    text_[len_++] = c;
    text_[len_] = 0;
    seq_++;
  }

  // The alphabet's own text for a code, or null.
  static const char* extra(uint32_t code) {
    const Sign* s = nullptr;
    int n = 0;
    if (alphabet_ == ACCENTED) s = ACCENTED_SIGNS, n = sizeof(ACCENTED_SIGNS) / sizeof(Sign);
    else if (alphabet_ == CYRILLIC) s = CYRILLIC_SIGNS, n = sizeof(CYRILLIC_SIGNS) / sizeof(Sign);
    for (int i = 0; i < n; i++)
      if (encode(s[i].code) == code) return s[i].text;
    return nullptr;
  }

  void endChar() {
    if (code_ == 1) return;
    const char* t = extra(code_);
    // Six dits or more is the error sign, however many the sender keyed.
    if (!t) t = code_ >= 1u << 6 && !(code_ & (code_ - 1)) ? "<HH>" : "*";
    for (int i = 0; i < NSIGNS && *t == '*'; i++)
      if (codes_[i] == code_) t = SIGNS[i].text;
    while (*t) put(*t++);
    code_ = 1;
    spaced_ = false;
  }

  static float lerp(float a, float b, float k) { return a + (b - a) * k; }

  static void sort(float* v, int n) {  // insertion: n is small
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
        float t = v[j];
        v[j] = v[j - 1];
        v[j - 1] = t;
      }
  }

  // Retunes the mixer without a jump in its phase, which would dip the band-pass's output.
  void tune(float hz) {
    hz_ = hz;
    float w = 2 * 3.14159265f * hz / RATE;
    stepC_ = cosf(w);
    stepS_ = sinf(w);
  }

  void cutoff(float fc) {
    // The chirp channels are wider than the tone's: a narrow one is too slow to see a sweep.
    float side = fc > CHIRP_FC ? fc : CHIRP_FC;
    // Each channel's noise scales with its width; a channel that wasn't in use is found afresh.
    float shift = fc_ > 0 ? 10 * log10f(side / chFc_) : 0;
    int was = fc_ > 0 ? nCh_ : -1;
    fc_ = fc;
    chFc_ = side;
    lp_[0].set(fc, (float)RATE / DECIM, 0.5412f);
    lp_[1].set(fc, (float)RATE / DECIM, 1.3066f);
    for (auto& ch : chLp_) {
      ch[0].set(side, (float)RATE / DECIM, 0.5412f);
      ch[1].set(side, (float)RATE / DECIM, 1.3066f);
    }
    // The chirp channels overlap where each is down ~0.4dB, and reach CHIRP_HZ.
    chStep_ = CHIRP_STEP * side;
    nCh_ = (int)ceilf(CHIRP_HZ / chStep_ - 0.01f);
    if (nCh_ > CH) nCh_ = CH;
    for (int c = 0; c < NCH; c++) {
      if (abs(c - CH) <= was) {
        chFloor_[c] += shift;
      } else {
        for (Biquad& f : chLp_[c]) f.clear();
        chWarm_[c] = CH_WARM;
        chRecent_[c] = chPre_[c] = chOnset_[c] = chGap_[c] = 0;
        chIn_[c] = false;
      }
    }
    float w = 2 * 3.14159265f * chStep_ * DECIM / RATE;
    rotStepC_ = cosf(w);
    rotStepS_ = sinf(w);
  }

  // One sample through the mixer, and every DECIM through the low-pass.
  void mix(float x) {
    accI_ += x * oscC_;
    accQ_ -= x * oscS_;
    float c = oscC_ * stepC_ - oscS_ * stepS_;
    oscS_ = oscS_ * stepC_ + oscC_ * stepS_;
    oscC_ = c;
    if (++nAcc_ < DECIM) return;
    float i = accI_ / DECIM, q = accQ_ / DECIM;
    accI_ = accQ_ = 0;
    nAcc_ = 0;
    // Chirp channel +k is the audio mixed down k x chStep_ further (by rot^k), -k as far the
    // other way.
    float pc = 1, ps = 0;
    for (int k = 0; k <= nCh_; k++) {
      for (int sgn = k ? -1 : 1; sgn <= 1; sgn += 2) {
        float ci = i * pc + q * ps * sgn, cq = q * pc - i * ps * sgn;
        int c = CH + sgn * k;
        chLp_[c][0].run(ci, cq);
        chLp_[c][1].run(ci, cq);
        chEnv_[c] += ci * ci + cq * cq;
      }
      float t = pc * rotC_ - ps * rotS_;
      ps = ps * rotC_ + pc * rotS_;
      pc = t;
    }
    float t = rotC_ * rotStepC_ - rotS_ * rotStepS_;
    rotS_ = rotS_ * rotStepC_ + rotC_ * rotStepS_;
    rotC_ = t;
    lp_[0].run(i, q);
    lp_[1].run(i, q);
    zI_ = i;
    zQ_ = q;
    env_ += i * i + q * q;
    nEnv_++;
  }

  // Halfway between two levels (dB) in amplitude.
  static float halfway(float a, float b) {
    return 20 * log10f((powf(10, a / 20) + powf(10, b / 20)) / 2);
  }

  // The split of the last HIST lengths in `h` (`count` written so far, as a ring) into two
  // groups with the least spread (in log terms): their means in `lo` and `hi`, if they're
  // minRatio to maxRatio apart and each group has two at least.
  static bool split(const float* h, int count, float minRatio, float maxRatio, float& lo, float& hi) {
    float v[HIST];
    int n = count < HIST ? count : HIST;
    if (n < 6) return false;
    for (int i = 0; i < n; i++) v[i] = logf(h[i]);
    sort(v, n);
    float bestCost = 1e30f, a0 = 0, b0 = 0;
    for (int k = 2; k < n - 1; k++) {  // v[0..k) and v[k..n)
      float a = 0, b = 0, cost = 0;
      for (int i = 0; i < k; i++) a += v[i];
      for (int i = k; i < n; i++) b += v[i];
      a /= k;
      b /= n - k;
      for (int i = 0; i < n; i++) cost += (v[i] - (i < k ? a : b)) * (v[i] - (i < k ? a : b));
      if (cost < bestCost) {
        bestCost = cost;
        a0 = a;
        b0 = b;
      }
    }
    float ratio = expf(b0 - a0);
    if (ratio < minRatio || ratio > maxRatio) return false;
    lo = expf(a0);
    hi = expf(b0);
    return true;
  }

  // Noise, fading and the threshold make marks end early and start late, so they're heard
  // short and gaps long by the same amount: half what a gap within a letter outlasts a dit.
  void unskew() {
    if (nRawDit_ < SKEW_AFTER || nRawGap_ < SKEW_AFTER) return;
    skew_ = (rawGap_ - rawDit_) / 2;
    if (skew_ > dit_ / 3) skew_ = dit_ / 3;
    if (skew_ < -dit_ / 3) skew_ = -dit_ / 3;
  }

  void mark(float raw) {
    float ms = raw + skew_;
    hist_[nHist_++ % HIST] = ms;
    if (nHist_ >= 2 * HIST) nHist_ -= HIST;
    bool dah = ms > (dit_ + dah_) / 2;
    if (dah) {
      dah_ = lerp(dah_, ms, 0.3f);
      if (dah_ > 4.5f * dit_ || dah_ < 2 * dit_) dit_ = lerp(dit_, dah_ / 3, 0.5f);
    } else {
      dit_ = lerp(dit_, ms, 0.3f);
      if (dah_ > 4.5f * dit_ || dah_ < 2 * dit_) dah_ = lerp(dah_, dit_ * 3, 0.5f);
    }
    // A sudden change of speed, which the running estimates can't follow (every mark looks
    // like a dit), settles within a few letters this way.
    split(hist_, nHist_, 2, 5, dit_, dah_);
    dah = ms > (dit_ + dah_) / 2;
    if (dit_ < 20) dit_ = 20;  // 60 WPM
    if (dit_ > 300) dit_ = 300;  // 4 WPM
    if (!dah) {
      rawDit_ = nRawDit_++ ? lerp(rawDit_, raw, 0.2f) : raw;
      unskew();
    }
    push(dah);
  }

  void push(bool dah) {
    if (!code_) return;
    if (code_ < 1u << MAX_ELEMENTS) code_ = code_ << 1 | dah;
    else if (dah || code_ & (code_ - 1)) code_ = 0;  // too long to be anything: ends as '*'
    // else more dits of the error sign: it stays as it is
  }

  // A gap has lasted `raw` ms so far: ends the character, then the word, once it's long enough.
  void gapSoFar(float raw) {
    float g = (raw - skew_) / dit_;
    if (code_ != 1 && g > 2) endChar();
    if (!spaced_ && g > (charGap_ + wordGap_) / 2) {
      put(' ');
      spaced_ = true;
    }
  }

  // A mark of `mark` ms after a gap of `gap`: decoded, logged, and if that changed the speed
  // (or the letter gap) by a third, the latest marks that fit the new speed decoded again.
  void element(float gap, float mark) {
    if (nLog_ == LOG) {
      memmove(log_, log_ + 1, (LOG - 1) * sizeof(Element));
      nLog_--;
    }
    log_[nLog_++] = {gap, mark, code_ == 1 ? scrolled_ + len_ : NOT_FIRST};
    this->gap(gap);
    this->mark(mark);
    if (fabsf(dit_ - ditRef_) > ditRef_ / 3 || fabsf(charGap_ - charRef_) > charRef_ / 3) redecode();
  }

  void redecode() {
    ditRef_ = dit_;
    charRef_ = charGap_;
    // Back from the latest mark for as long as the marks and gaps fit the new speed; then
    // forward to the first that starts a character and is still in the text.
    int from = nLog_;
    while (from > 0) {
      const Element& e = log_[from - 1];
      float m = e.mark + skew_, g = e.gap - skew_;
      if (m < dit_ / 2 || m > 1.7f * dah_ || g < dit_ / 2) break;
      from--;
    }
    while (from < nLog_ && (log_[from].at == NOT_FIRST || log_[from].at < scrolled_)) from++;
    if (from >= nLog_ - 1) return;
    len_ = log_[from].at - scrolled_;
    text_[len_] = 0;
    code_ = 1;
    spaced_ = len_ == 0 || text_[len_ - 1] == ' ';
    for (int i = from; i < nLog_; i++) {
      if (i > from) gapSoFar(log_[i].gap);
      log_[i].at = code_ == 1 ? scrolled_ + len_ : NOT_FIRST;
      push(log_[i].mark + skew_ > (dit_ + dah_) / 2);
    }
    seq_++;
  }

  // A gap of `raw` ms just ended: sorts letter gaps from word gaps.
  void gap(float raw) {
    float g = (raw - skew_) / dit_;
    if (g < 2) {
      rawGap_ = nRawGap_++ ? lerp(rawGap_, raw, 0.2f) : raw;
      unskew();
      return;
    }
    if (g > 3 * wordGap_) return;  // a pause, not a word space: it would drag the estimate up
    gHist_[nGap_++ % HIST] = g;
    if (nGap_ >= 2 * HIST) nGap_ -= HIST;
    if (g > (charGap_ + wordGap_) / 2) wordGap_ = lerp(wordGap_, g, 0.2f);
    else charGap_ = lerp(charGap_, g, 0.2f);
    // Farnsworth letter gaps can be as long as plain word gaps: the running estimates would
    // take every one for a word gap.
    split(gHist_, nGap_, 1.4f, 5, charGap_, wordGap_);
    if (charGap_ < 2.5f) charGap_ = 2.5f;
    if (wordGap_ < 1.4f * charGap_) wordGap_ = 1.4f * charGap_;
  }

  // A mark was heard: learns which channels light up just before marks start (chirp), and
  // takes them in. Noise and other signals light them as much before a mark as in the gaps,
  // and the key's click both sides of the tone alike; a chirp lights one side.
  void onset() {
    if (nOnsets_ < 1000) nOnsets_++;
    // The first marks are heard late, while the threshold settles: every channel is lit before
    // them.
    if (nOnsets_ <= CHIRP_SKIP) return;
    // An average of all the marks at first, then of the latest.
    float k = 1.0f / (nOnsets_ - CHIRP_SKIP);
    if (k < CHIRP_K) k = CHIRP_K;
    float lift[NCH] = {};
    for (int c = CH - nCh_; c <= CH + nCh_; c++) {
      if (chWarm_[c]) continue;
      chOnset_[c] = lerp(chOnset_[c], chPre_[c], k);
      lift[c] = chOnset_[c] - chGap_[c];
    }
    // A chirp sweeps: every channel from the tone out to the farthest that lights up counts.
    // The one on the tone bridges from the chirp to the band-pass, which is slower to rise.
    int lo = CH, hi = CH;
    for (int c = CH - nCh_; c <= CH + nCh_; c++) {
      float ref = lift[2 * CH - c];
      if (ref < lift[CH] - CHIRP_BELOW_DB) ref = lift[CH] - CHIRP_BELOW_DB;
      float over = lift[c] - ref;
      if (c != CH && !chWarm_[c] && nOnsets_ >= CHIRP_AFTER && over > (chIn_[c] ? CHIRP_OUT_DB : CHIRP_IN_DB)) {
        if (c < lo) lo = c;
        if (c > hi) hi = c;
      }
    }
    for (int c = 0; c < NCH; c++) chIn_[c] = hi > lo && c >= lo && c <= hi && !chWarm_[c];
  }

  void block() {
    // Goertzel power per bin, in dB relative to full scale. The Hann window keeps a strong
    // whistle nearby out of the tone's bin.
    float p[BINS], sorted[BINS];
    for (int i = 0; i < BLOCK; i++) x_[i] = blk_[i] * win_[i];
    // Four bins at a time: each one's steps wait on its last, so alone they leave the FPU idle
    // (the same sums, so the same results; CW was most of the Digital engine's time).
    auto power = [&](int b, float s1, float s2) {
      float pw = (s1 * s1 + s2 * s2 - coef_[b] * s1 * s2) / (BLOCK * BLOCK / 16.0f);
      p[b] = 10 * log10f(pw + 1e-12f);
      sorted[b] = p[b];
    };
    int b = 0;
    for (; b + 4 <= BINS; b += 4) {
      float c0 = coef_[b], c1 = coef_[b + 1], c2 = coef_[b + 2], c3 = coef_[b + 3];
      float a1 = 0, a2 = 0, b1 = 0, b2 = 0, d1 = 0, d2 = 0, e1 = 0, e2 = 0;
      for (int i = 0; i < BLOCK; i++) {
        float x = x_[i];
        float a0 = x + c0 * a1 - a2, b0 = x + c1 * b1 - b2, d0 = x + c2 * d1 - d2, e0 = x + c3 * e1 - e2;
        a2 = a1, a1 = a0, b2 = b1, b1 = b0, d2 = d1, d1 = d0, e2 = e1, e1 = e0;
      }
      power(b, a1, a2);
      power(b + 1, b1, b2);
      power(b + 2, d1, d2);
      power(b + 3, e1, e2);
    }
    for (; b < BINS; b++) {
      float s1 = 0, s2 = 0, c = coef_[b];
      for (int i = 0; i < BLOCK; i++) {
        float s0 = x_[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
      }
      power(b, s1, s2);
    }
    // Median of the bank: what broadband noise gives.
    sort(sorted, BINS);
    float med = sorted[BINS / 2];
    // Each bin's score: its average lift over the median, less the lift it keeps even in the
    // gaps. A keyed tone scores about half its lift, noise a little, and a steady whistle
    // (which keeps its lift) nothing. The lift leaves out what the bin usually has over the
    // median while there's no tone: the mic's noise isn't flat, and its low bins would
    // otherwise look lifted, their flicker keyed.
    float keyed[BINS];
    int best = binLo_;
    bool warm = hops_ < WARM_HOPS;  // a whistle that's there from the start mustn't look keyed
    if (warm) hops_++;
    for (int b = 0; b < BINS; b++) {
      if (!open_) shape_[b] = lerp(shape_[b], p[b] - med, warm ? SHAPE_FAST : SHAPE_K);
      float lift = p[b] - med - shape_[b];
      if (lift < 0) lift = 0;
      score_[b] = lerp(score_[b], lift, open_ ? SCORE_K : SCORE_FAST);
      low_[b] = lerp(low_[b], lift, lift < low_[b] || warm ? LOW_DOWN : LOW_UP);
      keyed[b] = score_[b] - low_[b];
      if (b >= binLo_ && b <= binHi_ && keyed[b] > keyed[best]) best = b;
    }
    if (bin_ < 0 || keyed[best] > SWITCH_RATIO * keyed[bin_] + 1) {
      if (bin_ != best) afc_ = false;  // the band-pass's floor and peak carry over
      bin_ = best;
    }
    memcpy(sorted, keyed, sizeof(sorted));
    sort(sorted, BINS);
    conf_ = keyed[bin_] - sorted[BINS / 2];
    // The squelch. A mark counts if it opened at some time while the mark lasted, so the first
    // mark after a silence (which opens it) isn't cut short.
    open_ = conf_ >= CONF_DB;
    // Until marks fine-tune it, the mixer follows the peak of a parabola through the scores.
    float centre = TONE_LO + bin_ * TONE_STEP;
    if (!afc_) {
      float at = centre;
      if (bin_ > 0 && bin_ < BINS - 1) {
        float a = keyed[bin_ - 1], b = keyed[bin_], c = keyed[bin_ + 1], den = a - 2 * b + c;
        float off = den < 0 ? 0.5f * (a - c) / den : 0;  // bins from the centre
        if (off > 1) off = 1;  // a flat top would throw it anywhere
        if (off < -1) off = -1;
        at += off * TONE_STEP;
      }
      if (fabsf(at - hz_) > 2) tune(at);
    }

    // The band-pass's power over this hop, scaled so a full-scale sine reads 0dB as the bins
    // do; or a chirp channel's, as far over the floor as it is over its own, where that's
    // higher: a chirping mark is heard from its start.
    float chLv[NCH] = {}, rel[NCH] = {};
    for (int c = CH - nCh_; c <= CH + nCh_; c++) {
      chLv[c] = 10 * log10f(4 * chEnv_[c] / (nEnv_ ? nEnv_ : 1) + 1e-12f);
      chEnv_[c] = 0;
      if (chWarm_[c]) {  // its filter is still filling: its floor just follows
        chWarm_[c]--;
        chFloor_[c] = chLv[c];
      }
      rel[c] = chLv[c] - chFloor_[c];
    }
    float lv = level_ = 10 * log10f(4 * env_ / (nEnv_ ? nEnv_ : 1) + 1e-12f);
    env_ = 0;
    nEnv_ = 0;
    // A tone stands as far over the band-pass's floor as over a chirp channel's, and more by
    // how much wider (noisier) that is.
    float wider = 10 * log10f(chFc_ / fc_);
    int loud = -1;  // the chirp channel it came from, if one
    for (int c = CH - nCh_; c <= CH + nCh_; c++)
      if (chIn_[c] && primed_ && floor_ + wider + rel[c] > lv) lv = floor_ + wider + rel[c], loud = c;
    // Each floor follows its channel while it has only noise in it, else creeps up (~4s), as
    // floor_ does. Not by the key: a mark the key missed would lift it, and hide the next.
    if (!clickHops_)
      for (int c = CH - nCh_; c <= CH + nCh_; c++)
        if (!chWarm_[c]) chFloor_[c] = lerp(chFloor_[c], chLv[c], rel[c] < MIN_THR_DB ? 0.04f : 0.003f);
    float n = oscC_ * oscC_ + oscS_ * oscS_;  // keep the oscillators on the unit circle
    oscC_ *= 1.5f - 0.5f * n;
    oscS_ *= 1.5f - 0.5f * n;
    n = rotC_ * rotC_ + rotS_ * rotS_;
    rotC_ *= 1.5f - 0.5f * n;
    rotS_ *= 1.5f - 0.5f * n;
    // AFC: the phase turned by (offset x hop) while the key was down. Not during a chirp, nor
    // just after it: the tone it settles on is the one to follow.
    sinceChirp_ = loud >= 0 && loud != CH ? 0 : sinceChirp_ < 255 ? sinceChirp_ + 1 : 255;
    if (down_ && raw_ && open_ && runHops_ > DEBOUNCE + 2 && primed_ && peak_ - floor_ >= MIN_SNR_DB && sinceChirp_ > CHIRP_SETTLE) {
      float re = zI_ * pI_ + zQ_ * pQ_, im = zQ_ * pI_ - zI_ * pQ_;
      float off = atan2f(im, re) * RATE / (2 * 3.14159265f * HOP);
      float to = hz_ + AFC_K * off;
      if (to < TONE_LO + binLo_ * TONE_STEP) to = TONE_LO + binLo_ * TONE_STEP;
      if (to > TONE_LO + binHi_ * TONE_STEP) to = TONE_LO + binHi_ * TONE_STEP;
      tune(to);
      bin_ = (int)lroundf((to - TONE_LO) / TONE_STEP);  // a drifting tone takes its bin along
      afc_ = true;
    }
    pI_ = zI_;
    pQ_ = zQ_;
    // A click or knock raises every bin at once, a tone only (mostly) its own.
    bool click = med > medAvg_ + CLICK_DB && p[bin_] - med < 6;
    if (click) clickHops_ = CLICK_HOLD;
    else if (clickHops_) clickHops_--;
    else medAvg_ = medAvg_ < -150 ? med : lerp(medAvg_, med, 0.02f);
    if (!primed_) {
      floor_ = peak_ = lv;
      primed_ = true;
    }
    // Floor: the average level while the key is up (~0.1s), creeping up slowly (~4s) while
    // it's down. An average, not the dips: noise through the band-pass swings 10dB and more.
    // Peak: up at once, down slowly (~1.5s), never below the floor. With no tone (squelch shut)
    // it hardly comes down (~15s), so a pause doesn't bring the threshold down to the room's
    // rumble.
    float thr = (floor_ + peak_) / 2;
    if (!clickHops_) {
      floor_ = lerp(floor_, level_, lv < thr ? 0.04f : 0.001f);  // the tone's channel's
      peak_ = lv > peak_ ? lerp(peak_, lv, 0.5f) : lerp(peak_, lv, open_ ? 0.003f : 0.0003f);
    }
    if (peak_ < floor_) peak_ = floor_;
    thr = halfway(floor_, peak_);
    if (thr < floor_ + MIN_THR_DB) thr = floor_ + MIN_THR_DB;  // noise alone seldom gets there
    bool ok = peak_ - floor_ >= MIN_SNR_DB;
    bool d = clickHops_ ? raw_ : ok && (raw_ ? lv > thr - HYST_DB : lv > thr + HYST_DB);
    rawRun_ = d == raw_ ? rawRun_ + 1 : 1;
    // Chirp: what each channel had just before a mark started, against what it has in the gaps.
    if (d && !raw_ && !down_) memcpy(chPre_, chRecent_, sizeof(chPre_));
    raw_ = d;
    for (int c = CH - nCh_; c <= CH + nCh_; c++) {
      if (chWarm_[c]) continue;
      float r = clickHops_ ? -100 : rel[c];  // a click lights them all: not a chirp
      chRecent_[c] = r > chRecent_[c] - CHIRP_FALL ? r : chRecent_[c] - CHIRP_FALL;
      if (!down_ && !raw_) chGap_[c] = lerp(chGap_[c], rel[c], 0.01f);
    }

    // The band-pass follows the speed.
    float fc = FC_PER_DIT * 1000 / dit_;
    if (fc < FC_MIN) fc = FC_MIN;
    if (fc > FC_MAX) fc = FC_MAX;
    if (fabsf(fc - fc_) > 0.1f * fc_) cutoff(fc);

    uint32_t debounce = (uint32_t)(dit_ / 4 / HOP_MS);
    if (debounce < DEBOUNCE) debounce = DEBOUNCE;
    runHops_++;
    if (raw_ != down_ && rawRun_ >= debounce) {
      // The change happened rawRun_ hops ago.
      uint32_t hops = runHops_ - rawRun_;
      if (!down_) {
        gapHops_ = hops;
        heard_ = false;
        runHops_ = rawRun_;
      } else if (heard_ || open_) {
        onset();
        element(gapHops_ * HOP_MS, hops * HOP_MS);
        runHops_ = rawRun_;
      } else {
        runHops_ += gapHops_;  // noise, not a mark: the gap goes on
      }
      down_ = raw_;
    }
    if (down_ && open_) heard_ = true;
    if (!down_) gapSoFar(runHops_ * HOP_MS);
  }
};
