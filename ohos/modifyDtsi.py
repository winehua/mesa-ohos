#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Copyright (c) 2022 Huawei Device Co., Ltd.

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from ast import Delete
import sys, os

class Modify():
    def __init__(self, filePath) -> None:
        self.filePath = filePath
        self.lines = []

        fp = open(self.filePath, 'r')
        for line in fp:
            self.lines.append(line)
        fp.close()

    def saveAndClose(self):
        s = ''.join(self.lines)
        self.lines.clear()
        fp = open(self.filePath, 'w')
        fp.write(s)
        fp.close()
    
    def positionSearch(self, positionText, startLineNum = 0):
        linesLen = len(self.lines)
        for lineNum in range(startLineNum + 1, linesLen):
            if(positionText in self.lines[lineNum]):
                return lineNum
        return -1

    def addItem(self, newText, positionInedx, rowNume):
        assert rowNume >= 0,"rowNum含义为“要将newText添加到positionSearch所定位的条目下的第几行”，可以等于0及添加到当前行，将当前行往下挤，但不可以为负数，如需要在positionSearch所定位的条目上方添加复数行，请按顺序调用addItem函数，且rowNum都填入0"
        item = ''
        if("is not set" in newText):
            item = newText[2:-11]
        else:
            item = newText[:-2]

        # 如果要添加的内容已经存在则直接返回
        for line in self.lines:
            if(item in line):
                if ((item + "=y" in line) or (item + " is not set" in line)):
                    print("Already has this item: " + item)
                    return

        assert positionInedx >= 0, "If want to add an item, first 'positionInedx' must be >= 0"
        lineNum = positionInedx + rowNume
        self.lines.insert(lineNum, newText+'\n')
        if(rowNume <= 0):
            positionInedx += 1
        print("Add item: "+item)

    def deleteLines(self, startLineIndex, endLineIndex):
        for i in range(endLineIndex - startLineIndex):
            self.lines.pop(startLineIndex)


if __name__ == "__main__":
    path = sys.argv[1]
    modify = Modify(path)
    print("Start to modifying file", sys.argv[1])

    test = ['		compatible = "rockchip, rk3568-mali", "arm,mali-bifrost";',
            '		reg = <0x0 0xfde60000 0x0 0x20000>;',
            '',
            '		interrupts = <GIC_SPI 40 IRQ_TYPE_LEVEL_HIGH>,',
            '				<GIC_SPI 41 IRQ_TYPE_LEVEL_HIGH>,',
            '				<GIC_SPI 39 IRQ_TYPE_LEVEL_HIGH>;',
            '		interrupt-names = "job", "mmu", "gpu";',
            '		clocks = <&scmi_clk 1>, <&cru CLK_GPU>;',
            '		clock-names = "core", "bus";',
            '		operating-points-v2 = <&gpu_opp_table>;',
            '',
            '		#cooling-cells = <2>;',
            '		power-domains = <&power RK3568_PD_GPU>;',
            '		status = "disabled";']

    if(modify.positionSearch("gpu_power_model: power-model",0) == -1):
        print("rk3568.dtsi arealy modified")
        exit()

    positionInedx1 = modify.positionSearch("gpu: gpu@fde60000",0)
    if(positionInedx1 >= 0):
        positionInedx2 = modify.positionSearch("};",positionInedx1)
        modify.deleteLines(positionInedx1 + 1, positionInedx2 + 1)

        for i in range(len(test)):
            modify.addItem(test[i], positionInedx1, i + 1)
    
    modify.saveAndClose()

    print("Succese")
