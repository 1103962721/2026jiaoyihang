"""Compare rendered production widgets to geometry measured from the approved preview."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
expected = json.loads((ROOT / 'Tests/Fixtures/ServerPurchasePreviewBounds.json').read_text())
actual = json.loads((ROOT / 'build/server-purchase-runtime/server-ui-bounds.json').read_text())
results = {}
for name, box in expected.items():
    assert name in actual, f'Missing UI element: {name}'
    deltas = [abs(a - b) for a, b in zip(box, actual[name])]
    results[name] = {'expected': box, 'actual': actual[name], 'maxPixelDifference': max(deltas)}
    assert max(deltas) <= 1, f'{name} differs from approved preview: {results[name]}'
report = {'passed': True, 'tolerancePixels': 1, 'elements': results,
          'note': 'Window controls and text rasterization use native Qt/Windows rendering.'}
(ROOT / 'build/server-purchase-runtime/ui-comparison-report.json').write_text(json.dumps(report, indent=2))
print(f'PASS approved preview geometry: {len(results)} regions; maximum difference {max(r["maxPixelDifference"] for r in results.values()):.3f}px')
