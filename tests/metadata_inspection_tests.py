"""Parser regressions use invented metadata, not proprietary binary fixtures."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('inspect_aimlabs',ROOT/'tools/inspect_aimlabs.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
class MetadataTests(unittest.TestCase):
    def test_literal_and_name_offsets(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build',prefix='metadata-test-') as folder:
            root=Path(folder); self.assertTrue(root.resolve().is_relative_to((ROOT/'build').resolve()))
            names=b'CorsairMouseHardwareAdapter\0'; literal=b'PureVector'
            header=struct.pack('<8I',0xfab11baf,29,32,8,40,len(literal),40+len(literal),len(names))
            path=root/'metadata.dat'; path.write_bytes(header+struct.pack('<II',len(literal),0)+literal+names)
            result=module.inspect_metadata(path)
            self.assertEqual({h['kind']:h['offset'] for h in result['hits']},{'literal':40,'name_pool':50})
            self.assertEqual(result['selected_types'],[])
            invalid=bytearray(path.read_bytes()); struct.pack_into('<I',invalid,28,0xffffffff); path.write_bytes(invalid)
            with self.assertRaises(ValueError): module.inspect_metadata(path)
if __name__=='__main__': unittest.main()
