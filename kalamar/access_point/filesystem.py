# -*- coding: utf-8 -*-
# This file is part of Dyko
# Copyright © 2008-2009 Kozea
#
# This library is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Kalamar.  If not, see <http://www.gnu.org/licenses/>.

import os.path
import re
from .base import AccessPoint
from ..item import Item
from ..property import Property


class FileSystem(AccessPoint):
    """Store each item in a file.

    """
    def __init__(self, root_dir, pattern, properties,
                 content_property='content'):
        if pattern.count('*') != len(properties):
            raise ValueError('FileSystem must have as many properties as'
                             '* wildcards in pattern.')
        self.root_dir = unicode(root_dir)
        self.content_property = content_property

        self._ordered_properties = tuple(
            (p, Property(unicode)) if isinstance(p, basestring)
            else p # Assume a (name, Property_instance) tuple.
            for p in properties)
        self.properties = dict(self._ordered_properties)
        assert content_property not in self.properties
        self.properties[content_property] = Property(file)
        # All properties here are in the identity
        self.identity_properties = tuple(name for name, p in 
                                         self._ordered_properties)

        self._pattern_parts = unicode(pattern).split('/')

        properties_iter = iter(self.identity_properties)
        self.properties_per_path_part = tuple(
            (tuple(next(properties_iter) for i in xrange(part.count('*'))),
             re.compile('^%s$' % '(.*)'.join(map(re.escape, part.split('*')))),
             part.replace('*', '%s'))
            for part in self._pattern_parts)
    
    def _filename_for(self, item):
        return os.path.join(self.root_dir, *(
            template % tuple(unicode(item[p]) for p in props)
            for props, regexp, template in self.properties_per_path_part))
        
    def search(self, request):
        def defered_open(path):
            def loader():
                return (open(path, 'rb'),)
            return loader
        def walk(root, remaining_path_parts, previous_properties=()):
            props, regexp, template = remaining_path_parts[0]
            remaining_path_parts = remaining_path_parts[1:]
            for basename in os.listdir(root):
                match = regexp.match(basename)
                if not match:
                    continue
                properties = dict(zip(props, match.groups()))
                properties.update(previous_properties)
                path = os.path.join(root, basename)
                if remaining_path_parts and os.path.isdir(path):
                    for item in walk(path, remaining_path_parts, properties):
                        yield item
                if not remaining_path_parts and not os.path.isdir(path):
                    item = Item(self, properties, {self.content_property: 
                        defered_open(path)})
                    if request.test(item):
                        yield item
        return walk(self.root_dir, self.properties_per_path_part)
    
    def delete(self, item):
        raise NotImplementedError
    
    def save(self, item): 
        raise NotImplementedError
    