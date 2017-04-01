# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Classes for converting parsed doc content into markdown pages."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import inspect


def build_md_page(page_info):
  """Given a PageInfo object, return markdown for the page.

  Args:
    page_info: must be a `parser.FunctionPageInfo`, `parser.ClassPageInfo`, or
        `parser.ModulePageInfo`

  Returns:
    Markdown for the page

  Raises:
    ValueError: if `page_info` is an instance of an unrecognized class
  """
  if page_info.for_function():
    return _build_function_page(page_info)

  if page_info.for_class():
    return _build_class_page(page_info)

  if page_info.for_module():
    return _build_module_page(page_info)

  raise ValueError('Unknown Page Info Type: %s' % type(page_info))


def _build_function_page(page_info):
  """Given a FunctionPageInfo object Return the page as an md string."""
  parts = [
      '# {page_info.full_name}{page_info.signature}\n\n'.format(
          page_info=page_info)
  ]

  if page_info.aliases:
    parts.extend('### `%s%s`\n' % (name, page_info.signature)
                 for name in page_info.aliases)
    parts.append('\n')

  if page_info.defined_in:
    parts.append('\n\n')
    parts.append(str(page_info.defined_in))

  parts.append(page_info.guides)
  parts.append(page_info.doc.docstring)
  parts.append(_build_function_details(page_info.doc.function_details))
  parts.append(_build_compatibility(page_info.doc.compatibility))

  return ''.join(parts)


def _build_class_page(page_info):
  """Given a ClassPageInfo object Return the page as an md string."""
  parts = ['# {page_info.full_name}\n\n'.format(page_info=page_info)]

  if page_info.aliases:
    parts.extend('### `class %s`\n' % name for name in page_info.aliases)
    parts.append('\n')

  if page_info.defined_in is not None:
    parts.append('\n\n')
    parts.append(str(page_info.defined_in))

  parts.append(page_info.guides)
  parts.append(page_info.doc.docstring)
  parts.append(_build_function_details(page_info.doc.function_details))
  assert not page_info.doc.compatibility
  parts.append('\n\n')

  if page_info.classes:
    parts.append('## Child Classes\n')

    link_template = ('[`class {class_info.short_name}`]'
                     '({class_info.url})\n\n')
    class_links = sorted(
        link_template.format(class_info=class_info)
        for class_info in page_info.classes)

    parts.extend(class_links)

  if page_info.properties:
    parts.append('## Properties\n\n')
    for prop_info in sorted(page_info.properties):
      h3 = '<h3 id="{short_name}"><code>{short_name}</code></h3>\n\n'
      parts.append(h3.format(short_name=prop_info.short_name))

      parts.append(prop_info.doc.docstring)
      parts.append(_build_function_details(prop_info.doc.function_details))
      assert not prop_info.doc.compatibility
      parts.append('\n\n')

    parts.append('\n\n')

  if page_info.methods:
    parts.append('## Methods\n\n')
    for method_info in sorted(page_info.methods):
      h3 = ('<h3 id="{short_name}">'
            '<code>{short_name}{signature}</code>'
            '</h3>\n\n')
      parts.append(h3.format(**method_info.__dict__))

      parts.append(method_info.doc.docstring)
      parts.append(_build_function_details(method_info.doc.function_details))
      parts.append(_build_compatibility(method_info.doc.compatibility))
      parts.append('\n\n')
    parts.append('\n\n')

  if page_info.other_members:
    parts.append('## Class Members\n\n')

    # TODO(wicke): Document the value of the members,
    #              at least for basic types.

    h3 = '<h3 id="{short_name}"><code>{short_name}</code></h3>\n\n'
    others_member_headings = (h3.format(short_name=info.short_name)
                              for info in sorted(page_info.other_members))
    parts.extend(others_member_headings)

  return ''.join(parts)


def _build_module_page(page_info):
  """Given a ClassPageInfo object Return the page as an md string."""

  parts = ['# Module: {full_name}\n\n'.format(full_name=page_info.full_name)]

  if page_info.aliases:
    parts.extend('### Module `%s`\n' % name for name in page_info.aliases)
    parts.append('\n')

  if page_info.defined_in is not None:
    parts.append('\n\n')
    parts.append(str(page_info.defined_in))

  parts.append(page_info.doc.docstring)
  parts.append('\n\n')
  parts.append('## Members\n\n')

  # TODO(deannarubin): Make this list into a table.
  for member_info in page_info.members:
    if not member_info.is_link():
      parts.append('Constant ' + member_info.short_name)
      parts.append('\n\n')
      continue

    suffix = ''
    if inspect.isclass(member_info.obj):
      link_text = 'class ' + member_info.short_name
    elif inspect.isfunction(member_info.obj):
      link_text = member_info.short_name + '(...)'
    elif inspect.ismodule(member_info.obj):
      link_text = member_info.short_name
      suffix = ' module'

    if member_info.doc.brief:
      suffix = '%s: %s' % (suffix, member_info.doc.brief)

    parts.append('[`{link_text}`]({url}){suffix}'.format(
        link_text=link_text, url=member_info.url, suffix=suffix))
    parts.append('\n\n')

  if page_info.members:
    parts.pop()

  return ''.join(parts)


def _build_compatibility(compatibility):
  """Return the compatability section as an md string."""
  parts = []
  sorted_keys = sorted(compatibility.keys())
  for key in sorted_keys:

    value = compatibility[key]
    parts.append('\n\n#### %s compatibility\n%s\n' % (key, value))

  return ''.join(parts)


def _build_function_details(function_details):
  """Return the function details section as an md string."""
  parts = []
  for detail in function_details:
    sub = []
    sub.append('#### ' + detail.keyword + ':\n\n')
    sub.append(detail.header)
    for key, value in detail.items:
      sub.append('* <b>`%s`</b>:%s' % (key, value))
    parts.append(''.join(sub))

  return '\n'.join(parts)
