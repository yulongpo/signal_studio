"""Independent epoch-1200 FFSCN graph and strictly bounded weight extraction.

The graph uses installed TorchVision MobileNetV3 building blocks, with strides
confined to the frequency axis. External training modules are never imported.
Legacy pickle class names are mapped to inert containers only to extract tensor
state; they are not executable implementations of the external network.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

import torch
from torch import nn
from torch.nn import functional as functional
from torchvision.models.mobilenetv3 import (
    InvertedResidual,
    InvertedResidualConfig,
)
from torchvision.ops import Conv2dNormActivation, SqueezeExcitation


CHECKPOINT_SHA256 = (
    "acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0"
)
OUTPUT_NAMES = ("hm", "bw", "off")
INPUT_SHAPE = (1, 1, 10, 65536)


def sha256_file(path: Path) -> str:
    """Hash large assets without materializing the complete file."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class _LegacyContainer(nn.Module):
    """Passive deserialization target, deliberately without a forward method."""


def checkpoint_state(path: Path,
                     expected: nn.Module | None = None) -> dict[str, torch.Tensor]:
    """Extract only the frozen checkpoint's state without external imports.

    The historical asset stores DataParallel and module instances rather than a
    plain state dictionary. weights_only remains enabled, with a finite explicit
    allowlist. Unknown pickle globals and mismatched asset hashes are rejected.
    """
    if sha256_file(path) != CHECKPOINT_SHA256:
        raise ValueError("Checkpoint SHA-256 differs from the frozen profile")
    legacy_names = (
        "lib.carrier_detection_net_1D_direct_avg.CarrierDetectionNet",
        "lib.carrier_detection_net_1D_direct_avg.DepthwiseSeparableConv",
        "lib.carrier_detection_net_1D_direct_avg.AdaptiveAvgPool2dCustom",
        "lib.my_mobile_net.MobileNetV3",
        "lib.my_mobile_net.InvertedResidual",
        "lib.my_mobile_net.ConvNormActivation",
        "lib.my_mobile_net.SqueezeExcitation",
        "lib.my_mobile_net_1D.SqueezeExcitation",
        "lib.decoders.FPN",
    )
    standard_classes = (
        nn.DataParallel,
        nn.Sequential,
        nn.ModuleList,
        nn.ModuleDict,
        nn.Conv2d,
        nn.BatchNorm2d,
        nn.AdaptiveAvgPool2d,
        nn.ReLU,
        nn.Sigmoid,
        nn.Hardswish,
        nn.Hardsigmoid,
    )
    allowed = list(standard_classes)
    allowed.extend((_LegacyContainer, name) for name in legacy_names)
    with torch.serialization.safe_globals(allowed):
        checkpoint = torch.load(path, map_location="cpu", weights_only=True)
    if isinstance(checkpoint, nn.DataParallel):
        checkpoint = checkpoint.module
    if not isinstance(checkpoint, _LegacyContainer):
        raise ValueError("Expected the frozen legacy model container")
    if expected is not None:
        _validate_stored_operators(checkpoint, expected)
    state = checkpoint.state_dict()
    if not state or any(not isinstance(value, torch.Tensor)
                        for value in state.values()):
        raise ValueError("Checkpoint must contain only tensor state")
    return dict(state)


def _validate_stored_operators(stored: nn.Module, expected: nn.Module) -> None:
    """Check parameter-free stride/padding/normalization choices as well.

    A strict state dict alone cannot detect a changed stride or BatchNorm eps.
    Standard primitive modules serialized in the old checkpoint provide these
    choices as plain data; their external custom forward methods remain unused.
    """
    stored_modules = dict(stored.named_modules())
    for path, module in expected.named_modules():
        previous = stored_modules.get(path)
        if previous is None:
            raise ValueError(f"Checkpoint operator missing: {path}")
        fields = ()
        if isinstance(module, nn.Conv2d):
            fields = ("in_channels", "out_channels", "kernel_size", "stride",
                      "padding", "dilation", "groups", "padding_mode")
        elif isinstance(module, nn.BatchNorm2d):
            fields = ("num_features", "eps", "momentum", "affine",
                      "track_running_stats")
        elif isinstance(module, nn.AdaptiveAvgPool2d):
            fields = ("output_size",)
        elif isinstance(module, (nn.Hardsigmoid, nn.Hardswish,
                                 nn.ReLU, nn.Sigmoid)):
            if type(module) is not type(previous):
                raise ValueError(f"Checkpoint activation differs: {path}")
        elif isinstance(module, TimeMean):
            if getattr(previous, "output_size", None) != 1:
                raise ValueError(f"Checkpoint temporal pooling differs: {path}")
        for field in fields:
            if getattr(previous, field, None) != getattr(module, field):
                raise ValueError(f"Checkpoint operator differs: {path}.{field}")


class FrequencyBackbone(nn.Module):
    """MobileNetV3 Large reduced-tail features at nine frequency scales."""

    def __init__(self, down_nums: int = 11):
        super().__init__()
        if down_nums < 6:
            raise ValueError("MobileNet frequency pyramid requires six levels")
        # input, kernel, expanded, output, squeeze/excitation, activation, stride
        layout = (
            (16, 3, 16, 16, False, "RE", 1),
            (16, 3, 64, 24, False, "RE", 2),
            (24, 3, 72, 24, False, "RE", 1),
            (24, 5, 72, 40, True, "RE", 2),
            (40, 5, 120, 40, True, "RE", 1),
            (40, 5, 120, 40, True, "RE", 1),
            (40, 3, 240, 80, False, "HS", 2),
            (80, 3, 200, 80, False, "HS", 1),
            (80, 3, 184, 80, False, "HS", 1),
            (80, 3, 184, 80, False, "HS", 1),
            (80, 3, 480, 112, True, "HS", 1),
            (112, 3, 672, 112, True, "HS", 1),
            (112, 5, 672, 80, True, "HS", 2),
            (80, 5, 480, 80, True, "HS", 1),
            (80, 5, 480, 80, True, "HS", 1),
        )
        layout += ((80, 5, 480, 80, True, "HS", 2),) * (down_nums - 6)
        norm = lambda channels: nn.BatchNorm2d(
            channels, eps=0.001, momentum=0.01)
        layers = [Conv2dNormActivation(
            1, 16, kernel_size=3, stride=(1, 2),
            norm_layer=norm, activation_layer=nn.Hardswish)]
        for spec in layout:
            config = InvertedResidualConfig(*spec, dilation=1, width_mult=1.0)
            block = InvertedResidual(config, norm)
            # TorchVision supplies the standard published blocks. Only the
            # depthwise spatial stride differs for the time/frequency graph.
            for convolution in block.modules():
                if isinstance(convolution, nn.Conv2d):
                    convolution.stride = (1, convolution.stride[1])
            layers.append(block)
        self.features = nn.Sequential(*layers)
        self.output_indices = (3, 6, 12, 15) + tuple(
            range(16, len(layers)))
        self.out_channels = (24, 40, 112) + (80,) * (down_nums - 5)

    def forward(self, spectrum: torch.Tensor) -> list[torch.Tensor]:
        levels = []
        for index, layer in enumerate(self.features):
            spectrum = layer(spectrum)
            if index in self.output_indices:
                levels.append(spectrum)
        return levels


class FrequencyPyramid(nn.Module):
    """Two top-down sums with squeeze/excitation between the passes."""

    def __init__(self, channels: tuple[int, ...]):
        super().__init__()
        self.latlayers = nn.ModuleList(
            nn.Conv2d(count, 64, kernel_size=1, bias=False)
            for count in channels)
        self.smooth_layers = nn.ModuleList(
            SqueezeExcitation(64, 16) for _ in channels)

    @staticmethod
    def _combine(levels: list[torch.Tensor]) -> list[torch.Tensor]:
        combined = [levels[0]]
        for level in levels[1:]:
            upsampled = functional.interpolate(
                combined[-1], scale_factor=(1, 2), mode="bilinear",
                align_corners=False)
            combined.append(level + upsampled)
        return combined

    def forward(self, features: list[torch.Tensor]) -> list[torch.Tensor]:
        projected = [layer(feature) for layer, feature in
                     zip(self.latlayers, features, strict=True)][::-1]
        first_pass = self._combine(projected)
        gated = [layer(feature) for layer, feature in
                 zip(self.smooth_layers, first_pass, strict=True)]
        return self._combine(gated)[::-1]


class TimeMean(nn.Module):
    """Collapse ten time rows without changing frequency coordinates."""

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return value.mean(dim=2, keepdim=True)


class SeparableHeadConvolution(nn.Module):
    """Replication padding belongs only to the detection heads."""

    def __init__(self):
        super().__init__()
        self.depthwise = nn.Conv2d(
            64, 64, kernel_size=3, padding=1, padding_mode="replicate",
            groups=64, bias=False)
        self.pointwise = nn.Conv2d(64, 64, kernel_size=1, bias=False)

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return self.pointwise(self.depthwise(value))


def _make_head() -> nn.Sequential:
    return nn.Sequential(
        TimeMean(), SqueezeExcitation(64, 16), SeparableHeadConvolution(),
        nn.ReLU(), nn.Conv2d(64, 1, kernel_size=1, bias=False))


class FfscnEpoch1200(nn.Module):
    """Raw hm/bw/off only; product postprocessing stays outside the graph."""

    def __init__(self):
        super().__init__()
        self.backbone = FrequencyBackbone()
        self.neck = nn.ModuleDict({
            "FPN_0": FrequencyPyramid(self.backbone.out_channels)})
        self.reg_head = _make_head()
        self.bw_head = _make_head()
        self.off_head = _make_head()

    def forward(self, spectrum: torch.Tensor) -> tuple[torch.Tensor, ...]:
        feature = self.neck["FPN_0"](self.backbone(spectrum))[0]
        return (torch.sigmoid(self.reg_head(feature)),
                torch.relu(self.bw_head(feature)),
                torch.sigmoid(self.off_head(feature)))


def load_model(checkpoint: Path, device: str = "cpu") -> FfscnEpoch1200:
    """Require every state key and tensor shape to match; no partial loading."""
    model = FfscnEpoch1200()
    model.load_state_dict(checkpoint_state(checkpoint, model), strict=True)
    return model.eval().to(device)
