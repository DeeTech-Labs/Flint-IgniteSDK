"""Regression checks for actionable documentation validation, not rendered prose."""
import importlib.util
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_docs', ROOT/'tools/check_docs.py')
docs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(docs)

class DocumentationTests(unittest.TestCase):
    def setUp(self):
        (ROOT/'temp').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='docs-check-', dir=ROOT/'temp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_fenced_examples_do_not_create_links_or_anchors(self):
        text = '# Ввод и API\n```md\n# Invisible\n[example](absent.md)\n```\n'
        (self.root/'README.md').write_text(text, encoding='utf-8')
        self.assertEqual(docs.anchors(text), {'ввод-и-api'})
        self.assertEqual(docs.check_links(self.root, {}), [])

    def test_local_missing_target_anchor_and_json_fail(self):
        (self.root/'README.md').write_text('[bad](missing.md)\n[anchor](target.md#no)\n```json\n{"bad":}\n```\n', encoding='utf-8')
        (self.root/'target.md').write_text('# Present\n', encoding='utf-8')
        errors = docs.check_links(self.root, {})
        self.assertEqual(len(errors), 3)
        self.assertTrue(any('missing anchor' in e for e in errors))
        self.assertTrue(any('invalid json fence' in e for e in errors))

    def test_cross_repository_links_resolve_to_owning_checkout(self):
        other = self.root/'other'; other.mkdir()
        (other/'README.md').write_text('# Hello\n', encoding='utf-8')
        (self.root/'README.md').write_text('[ok](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/README.md#hello)\n[missing](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/no.md)\n', encoding='utf-8')
        errors = docs.check_links(self.root, {'Flint-Flare': other})
        self.assertEqual(len(errors),1)
        self.assertIn('no.md',errors[0])

    def test_unclosed_markdown_fence_fails(self):
        (self.root/'README.md').write_text('# Example\n```sh\ncmake --build build/debug\n', encoding='utf-8')
        self.assertTrue(any('unclosed Markdown fence' in e for e in docs.check_links(self.root, {})))

    @unittest.skipUnless((ROOT.parent/'FlinT-OS/tools/select_device.py').exists(), 'owning OS checkout required for real model-schema validation')
    def test_toml_example_uses_the_real_os_schema(self):
        source = self.root/'README.md'
        source.write_text('```toml\n[background]\nmax_services = 1\n```\n', encoding='utf-8')
        with patch.object(docs, 'documents', return_value=[source]):
            self.assertEqual(docs.check_model_fences(ROOT.parent/'FlinT-OS'), [])
            source.write_text('```toml\n[background]\nunknown_policy = 1\n```\n', encoding='utf-8')
            self.assertTrue(docs.check_model_fences(ROOT.parent/'FlinT-OS'))

    def test_generated_registry_tracks_error_and_permission_sources(self):
        (self.root/'spec').mkdir()
        (self.root/'include/flint').mkdir(parents=True)
        (self.root/'spec/services.json').write_text('{"services": []}')
        (self.root/'spec/permissions.json').write_text('{"version":1,"persistent":["sensors.temperature"]}')
        (self.root/'include/flint/contracts.h').write_text('ContractRevision = 0x20261114; enum class Error {\n ok,\n conflict\n};')
        self.assertEqual(docs.check_registry(self.root,write=True),[])
        self.assertEqual(docs.check_registry(self.root),[])
        (self.root/'spec/permissions.json').write_text('{"version":1,"persistent":[]}')
        self.assertEqual(len(docs.check_registry(self.root)),2)

if __name__ == '__main__':
    unittest.main()
