#!/usr/bin/env python3
"""Offline inference harness for the zh-base-ct2-int8 model.

Replicates the plugin's C++ inference path (ct2_backend.cc + context_builder.cc
post-processing) so decoding-parameter experiments here transfer 1:1 to the
plugin:

  ct2_input = window_text + "<pinyin_start>" + prompt + "</pinyin_start>"
  -> CT2 translate_batch (seq2seq, decoder start <hanzi_start>)
  -> Detokenize (skip specials, byte-level decode, strip spaces)
  -> ExtractDisplayText (strip punctuation)   # what the candidate menu shows

The tokenizer is the canonical HF `tokenizers` (the training-time tokenizer).
We verified it produces byte-identical tokens to the C++ port on real inputs
(hanzi are added tokens; pinyin goes through byte-level BPE; space -> U+0120).
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

import ctranslate2
from tokenizers import Tokenizer

# ---------------------------------------------------------------- defaults --
# Mirrors ct2_backend.cc constants + schema.fragment.yaml defaults.
DEFAULT_MODEL = Path.home() / "Library/Rime/predict_models/zh-base-ct2-int8"

# Bytes that are "direct" in GPT-2's bytes_to_unicode(): printable ASCII plus
# the Latin-1 supplement ranges, matching the C++ buildUnicodeToByteMap.
_DIRECT_BYTES = (
    list(range(ord("!"), ord("~") + 1))
    + list(range(ord("¡"), ord("¬") + 1))
    + list(range(ord("®"), ord("ÿ") + 1))
)
_DIRECT_SET = frozenset(_DIRECT_BYTES)


def _build_byte_maps() -> tuple[dict[int, int], dict[int, int]]:
    """Return (unicode_codepoint -> byte, byte -> unicode_codepoint)."""
    u2b: dict[int, int] = {}
    b2u: dict[int, int] = {}
    for b in _DIRECT_BYTES:
        u2b[b] = b
        b2u[b] = b
    n = 0
    for b in range(256):
        if b not in _DIRECT_SET:
            u2b[256 + n] = b
            b2u[b] = 256 + n
            n += 1
    return u2b, b2u


_UNICODE_TO_BYTE, _BYTE_TO_UNICODE = _build_byte_maps()


def byte_level_decode(text: str) -> str:
    """Reverse GPT-2 byte-level encoding back to UTF-8 bytes (ct2 byteLevelDecode)."""
    out = bytearray()
    for ch in text:
        cp = ord(ch)
        b = _UNICODE_TO_BYTE.get(cp)
        if b is not None:
            out.append(b)
        else:
            out.extend(ch.encode("utf-8"))
    return out.decode("utf-8", errors="replace")


# -------------------------------------------------------------------- punct --

def _is_punct_codepoint(cp: int) -> bool:
    if cp in (ord("."), ord(","), ord("!"), ord("?"), ord(";"), ord(":")):
        return True
    if 0x3000 <= cp <= 0x303F:  # CJK symbols & punctuation
        return True
    if (0xFF01 <= cp <= 0xFF0F) or (0xFF1A <= cp <= 0xFF20) or \
       (0xFF3B <= cp <= 0xFF40) or (0xFF5B <= cp <= 0xFF65):  # fullwidth ASCII
        return True
    if 0x2010 <= cp <= 0x205E:  # general punctuation
        return True
    return False


def strip_all_punctuation(text: str) -> str:
    return "".join(ch for ch in text if not _is_punct_codepoint(ord(ch)))


def is_displayable_candidate(display: str) -> bool:
    return not any(("a" <= c <= "z") or ("A" <= c <= "Z") for c in display)


# --------------------------------------------------------------------- model --

@dataclass
class InferenceOptions:
    """Decoding knobs, mirroring ctranslate2::TranslationOptions."""
    max_decoding_length: int = 64
    beam_size: int = 1
    length_penalty: float = 1.0
    repetition_penalty: float = 1.2
    sampling_topk: int = 1
    sampling_temperature: float = 1.0
    sampling_topp: float = 1.0
    num_hypotheses: int = 1
    min_decoding_length: int = 1
    no_repeat_ngram_size: int = 0


@dataclass
class Hypothesis:
    tokens: list[str]
    ids: list[int]
    raw: str      # Detokenize output (hanzi + punctuation)
    display: str  # after strip_all_punctuation
    score: float = 0.0


@dataclass
class Prediction:
    hypotheses: list[Hypothesis] = field(default_factory=list)


class Infer:
    def __init__(self, model_dir: Path, device: str = "cpu"):
        self.model_dir = Path(model_dir)
        self.translator = ctranslate2.Translator(str(self.model_dir), device=device)
        self.tok = Tokenizer.from_file(str(self.model_dir / "tokenizer.json"))
        self.tok.no_padding()
        self.tok.enable_truncation(max_length=128)
        # end token: </hanzi_start> (C++ also tries </s> but it's absent here).
        self._end_token = ["</hanzi_start>"]
        self._special = {
            "<hanzi_start>", "</hanzi_start>", "<s>", "</s>", "<pad>",
            "<|endoftext|>", "<|im_start|>", "<|im_end|>", "<|PAD|>",
            "<|EOS|>", "<|BOS|>", "[CLS]", "[SEP]", "[PAD]", "[UNK]",
        }

    def encode(self, text: str) -> list[str]:
        return self.tok.encode(text).tokens

    def predict(self, ct2_input: str, opt: InferenceOptions | None = None,
                suppress_sequences: list[list[str]] | None = None) -> Prediction:
        opt = opt or InferenceOptions()
        src_tokens = self.encode(ct2_input)
        results = self.translator.translate_batch(
            [src_tokens],
            beam_size=opt.beam_size,
            num_hypotheses=opt.num_hypotheses,
            max_decoding_length=opt.max_decoding_length,
            min_decoding_length=opt.min_decoding_length,
            length_penalty=opt.length_penalty,
            repetition_penalty=opt.repetition_penalty,
            no_repeat_ngram_size=opt.no_repeat_ngram_size,
            sampling_topk=opt.sampling_topk,
            sampling_temperature=opt.sampling_temperature,
            sampling_topp=opt.sampling_topp,
            end_token=self._end_token,
            suppress_sequences=suppress_sequences,
            return_scores=opt.num_hypotheses > 1,
        )
        r = results[0]
        pred = Prediction()
        for i, toks in enumerate(r.hypotheses):
            raw = self._detokenize(toks)
            score = r.scores[i] if r.scores else 0.0
            pred.hypotheses.append(Hypothesis(
                tokens=toks, ids=[], raw=raw,
                display=strip_all_punctuation(raw), score=score,
            ))
        return pred

    def _detokenize(self, toks: list[str]) -> str:
        raw = "".join(t for t in toks if t not in self._special)
        return byte_level_decode(raw).replace(" ", "")


def make_input(window_text: str, prompt: str) -> str:
    return f"{window_text}<pinyin_start>{prompt}</pinyin_start>"


# ---------------------------------------------------------------------- CLI --

def _main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("inputs", nargs="*", help="ct2_input strings, or @FILE with one per line")
    ap.add_argument("--model", default=str(DEFAULT_MODEL))
    ap.add_argument("--beam", type=int, default=1)
    ap.add_argument("--num-hypotheses", type=int, default=1)
    ap.add_argument("--max-tokens", type=int, default=64)
    ap.add_argument("--length-penalty", type=float, default=1.0)
    ap.add_argument("--repetition-penalty", type=float, default=1.2)
    ap.add_argument("--topk", type=int, default=1)
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--no-repeat-ngram", type=int, default=0)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    inputs: list[str] = []
    for a in args.inputs:
        if a.startswith("@"):
            inputs.extend(line.rstrip("\n") for line in open(a[1:], encoding="utf-8") if line.strip())
        else:
            inputs.append(a)

    inf = Infer(Path(args.model))
    opt = InferenceOptions(
        max_decoding_length=args.max_tokens, beam_size=args.beam,
        num_hypotheses=args.num_hypotheses, length_penalty=args.length_penalty,
        repetition_penalty=args.repetition_penalty, sampling_topk=args.topk,
        sampling_temperature=args.temperature, no_repeat_ngram_size=args.no_repeat_ngram,
    )

    out = []
    for src in inputs:
        pred = inf.predict(src, opt)
        rows = [{"raw": h.raw, "display": h.display, "score": h.score} for h in pred.hypotheses]
        out.append({"input": src, "hypotheses": rows})
        if not args.json:
            print(f"IN:  {src}")
            for h in pred.hypotheses:
                print(f"  raw={h.raw!r}  display={h.display!r}  score={h.score:.4f}")
    if args.json:
        print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
